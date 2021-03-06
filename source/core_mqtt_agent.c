/*
 * FreeRTOS version 202012.00-LTS
 * Copyright (C) 2020 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * http://aws.amazon.com/freertos
 * http://www.FreeRTOS.org
 */

/**
 * @brief Implementation for MQTT agent APIs.
 * MQTT agent contains a dedicated FreeRTOS task which runs in a loop and process MQTT operations from
 * application tasks. A queue is used by the application tasks to enqueue an MQTT operation to be picked
 * up by the MQTT agent task. MQTT agent task calls the corresponding MQTT library API and adds it to a pending
 * operations list if the operation requires an acknowledgment. It receives MQTT packets from the network, if there
 * are no operations in queue to be processed.
 */



#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "stream_buffer.h"
#include "queue.h"
#include "semphr.h"

#include "fsl_debug_console.h"

#include "core_mqtt_agent.h"

/**
 * @brief Task priority for MQTT agent is set to higher priority than other tasks.
 */
#define MQTT_AGENT_TASK_PRIORITY                ( configMAX_PRIORITIES - 1 )

/**
 * @brief Stack size of the MQTT agent, will be the minimum size required by the MQTT library
 * and the underlying transport interface.
 */
#define MQTT_AGENT_TASK_STACK_SIZE              ( 2048 )

/**
 * @brief Maximum number of concurrent operations for MQTT agent.
 */
#define MQTT_AGENT_MAX_CONCURRENT_OPERATIONS    ( 5 )

/**
 * @brief Maximum polling interval for the agent. The agent will be listening on incoming messages during
 * this interval.
 */
#define MQTT_AGENT_MAX_POLLING_INTERVAL_MS      ( 500 )

/**
 * @brief Function used to add a MQTT operation to the pending list for receiving ACKS from broker.
 *
 * @param[in] pOperation Pointer to the operation pending.
 * @return pdTRUE if the operation was enqueued, pdFALSE if max pending operations are reached.
 */
static BaseType_t addPendingOperation( MQTTOperation_t * pOperation );

/**
 * @brief Pops pending MQTT operation with the packet identifier.
 *
 * @param[in] packetIndentifier The packet identifier for the pending MQTT operation.
 * @return Pointer to the MQTT operation poped, NULL if there are no operations with that packet identifier.
 */
static MQTTOperation_t * getPendingOperation( uint16_t packetIdentifier );

/**
 * @brief Main agent task loop.
 * Agent runs in a loop processing MQTT operations from application tasks. It exits loop on explicitly calling
 * MQTTAgent_Stop() from application tasks.
 *
 * @param[in] pParams Paramaters for the agent task.
 */
static void prvMQTTAgentLoop( void * pParams );

/**
 * @brief The default operation used when there are no other operations in queue.
 */
static MQTTOperation_t receiveOP =
{
    .type = MQTT_OP_RECEIVE
};

/**
 * @brief Queue used to receive MQTT operations to be processed by MQTT agent.
 */
static QueueHandle_t xOperationsQueue;

/**
 * @brief Static array used to keep track of pending MQTT operations that require an ACK to be received from broker.
 */
static MQTTOperation_t * pendingOperations[ MQTT_AGENT_MAX_CONCURRENT_OPERATIONS ];

/**
 * @brief Variable used to check if the agent is running.
 */
static BaseType_t isAgentRunning = pdFALSE;


static BaseType_t addPendingOperation( MQTTOperation_t * pOperation )
{
    size_t index = 0;
    BaseType_t result = pdFALSE;

    for( index = 0; index < MQTT_AGENT_MAX_CONCURRENT_OPERATIONS; index++ )
    {
        if( pendingOperations[ index ] == NULL )
        {
            pendingOperations[ index ] = pOperation;
            result = pdTRUE;
            break;
        }
    }

    return result;
}

static MQTTOperation_t * getPendingOperation( uint16_t packetIdentifier )
{
    size_t index = 0;
    MQTTOperation_t * pOperation = NULL;


    for( index = 0; index < MQTT_AGENT_MAX_CONCURRENT_OPERATIONS; index++ )
    {
        if( ( pendingOperations[ index ] != NULL ) &&
            ( pendingOperations[ index ]->packetIdentifier == packetIdentifier ) )
        {
            pOperation = pendingOperations[ index ];
            pendingOperations[ index ] = NULL;
            break;
        }
    }

    return pOperation;
}


static void prvMQTTAgentLoop( void * pParams )
{
    BaseType_t status;
    MQTTOperation_t * pOperation;
    uint16_t packetIdentifier = 0;
    MQTTContext_t * pMQTTContext = ( MQTTContext_t * ) pParams;
    MQTTStatus_t mqttStatus;

    isAgentRunning = pdTRUE;

    for( ; ; )
    {
        status = xQueueReceive( xOperationsQueue, &pOperation, 1 );

        if( status == pdTRUE )
        {
            switch( pOperation->type )
            {
                case MQTT_OP_RECEIVE:
                    mqttStatus = MQTT_ProcessLoop( pMQTTContext, MQTT_AGENT_MAX_POLLING_INTERVAL_MS );
                    configASSERT( mqttStatus == MQTTSuccess );
                    xQueueSend( xOperationsQueue, &pOperation, 1 );
                    break;

                case MQTT_OP_PUBLISH:

                    if( pOperation->info.pPublishInfo->qos != MQTTQoS0 )
                    {
                        packetIdentifier = MQTT_GetPacketId( pMQTTContext );
                    }
                    else
                    {
                        packetIdentifier = 0;
                    }

                    mqttStatus = MQTT_Publish( pMQTTContext, pOperation->info.pPublishInfo, packetIdentifier );

                    if( ( mqttStatus != MQTTSuccess ) || ( pOperation->info.pPublishInfo->qos == MQTTQoS0 ) )
                    {
                        pOperation->callback( pOperation, mqttStatus );
                    }
                    else
                    {
                        pOperation->packetIdentifier = packetIdentifier;
                        configASSERT( addPendingOperation( pOperation ) == pdTRUE );
                    }

                    break;

                case MQTT_OP_SUBSCRIBE:
                    packetIdentifier = MQTT_GetPacketId( pMQTTContext );
                    mqttStatus = MQTT_Subscribe( pMQTTContext,
                                                 pOperation->info.subscriptionInfo.pSubscriptionList,
                                                 pOperation->info.subscriptionInfo.numSubscriptions,
                                                 packetIdentifier );

                    if( mqttStatus != MQTTSuccess )
                    {
                        pOperation->callback( pOperation, mqttStatus );
                    }
                    else
                    {
                        pOperation->packetIdentifier = packetIdentifier;
                        configASSERT( addPendingOperation( pOperation ) == pdTRUE );
                    }

                    break;

                case MQTT_OP_UNSUBSCRIBE:
                    packetIdentifier = MQTT_GetPacketId( pMQTTContext );
                    mqttStatus = MQTT_Unsubscribe( pMQTTContext,
                                                   pOperation->info.subscriptionInfo.pSubscriptionList,
                                                   pOperation->info.subscriptionInfo.numSubscriptions,
                                                   packetIdentifier );

                    if( mqttStatus != MQTTSuccess )
                    {
                        pOperation->callback( pOperation, mqttStatus );
                    }
                    else
                    {
                        pOperation->packetIdentifier = packetIdentifier;
                        configASSERT( addPendingOperation( pOperation ) == pdTRUE );
                    }

                    break;

                case MQTT_OP_STOP:
                    /* Reset the operations queue to empty state to stop the agent. */
                    xQueueReset( xOperationsQueue );
                    pOperation->callback( pOperation, MQTTSuccess );
                    break;

                default:
                    break;
            }
        }
        else
        {
            break;
        }
    }

    vQueueDelete( xOperationsQueue );

    isAgentRunning = pdFALSE;

    vTaskDelete( NULL );
}

BaseType_t MQTTAgent_Init( MQTTContext_t * pMqttContext )
{
    BaseType_t result = pdTRUE;
    MQTTOperation_t * pOperation = &receiveOP;

    memset( pendingOperations, 0x00, sizeof( pendingOperations ) );

    if( result == pdTRUE )
    {
        xOperationsQueue = xQueueCreate( MQTT_AGENT_MAX_CONCURRENT_OPERATIONS, sizeof( MQTTOperation_t * ) );

        if( xOperationsQueue == NULL )
        {
            PRINTF( "MQTT Agent failed to create the queue.\r\n" );
            result = pdFALSE;
        }
    }

    if( result == pdTRUE )
    {
        result = xQueueSend( xOperationsQueue, &pOperation, 1 );
    }

    if( result == pdTRUE )
    {
        if( ( result = xTaskCreate( prvMQTTAgentLoop,
                                    "MQTT_Agent_task",
                                    MQTT_AGENT_TASK_STACK_SIZE,
                                    pMqttContext,
                                    MQTT_AGENT_TASK_PRIORITY | portPRIVILEGE_BIT,
                                    NULL ) ) != pdTRUE )
        {
            PRINTF( "Failed to create MQTT Agent task.\r\n" );
        }
    }

    return result;
}


BaseType_t MQTTAgent_ProcessEvent( MQTTContext_t * pMQTTContext,
                                   struct MQTTPacketInfo * pPacketInfo,
                                   struct MQTTDeserializedInfo * pDeserializedInfo )
{
    BaseType_t result = pdFALSE;
    MQTTOperation_t * pOperation;

    if( pDeserializedInfo->deserializationResult == MQTTSuccess )
    {
        switch( pPacketInfo->type )
        {
            case MQTT_PACKET_TYPE_PUBACK:
            case MQTT_PACKET_TYPE_SUBACK:
            case MQTT_PACKET_TYPE_UNSUBACK:
                pOperation = getPendingOperation( pDeserializedInfo->packetIdentifier );

                if( pOperation != NULL )
                {
                    pOperation->callback( pOperation, MQTTSuccess );
                    result = pdTRUE;
                }

                break;

            default:
                break;
        }
    }

    return result;
}

void MQTTAgent_Stop( void )
{
    MQTTOperation_t operation = { 0 };

    operation.type = MQTT_OP_STOP;

    xQueueSend( xOperationsQueue, &operation, portMAX_DELAY );

    while( isAgentRunning == pdTRUE )
    {
        vTaskDelay( pdMS_TO_TICKS( 1000 ) );
    }
}


BaseType_t MQTTAgent_Enqueue( MQTTOperation_t * pOperation,
                              TickType_t timeoutTicks )
{
    return xQueueSend( xOperationsQueue, &pOperation, timeoutTicks );
}
