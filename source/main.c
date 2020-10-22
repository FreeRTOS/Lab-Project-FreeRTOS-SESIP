/*
 * Copyright (c) 2015, Freescale Semiconductor, Inc.
 * Copyright 2016-2017 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* FreeRTOS kernel includes. */
#include "LPC54018.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "timers.h"

/* Freescale includes. */
#include "fsl_device_registers.h"
#include "fsl_debug_console.h"
#include "board.h"

#include "pin_mux.h"
#include <stdbool.h>


#include "FreeRTOS_IP.h"
#include "FreeRTOS_Sockets.h"
#include "NetworkInterface.h"

#include "plaintext_freertos.h"
/*******************************************************************************
 * Definitions
 ******************************************************************************/

static const uint8_t ucIPAddress[ 4 ] = { 192,168,86,43 };
static const uint8_t ucNetMask[ 4 ] = { 255,255,255,0 };
static const uint8_t ucGatewayAddress[ 4 ] = { 192,168,86,1 };
static const uint8_t ucDNSServerAddress[ 4 ] = { 192,168,86,1 };
static const uint8_t ucMACAddress[6] = { 0xDE, 0xAD, 0x00, 0xBE, 0xEF, 0x00};

/* Task priorities. */
#define hello_task_PRIORITY (configMAX_PRIORITIES - 1)
/*******************************************************************************
 * Prototypes
 ******************************************************************************/
static void hello_task(void *pvParameters);

static UBaseType_t ulNextRand = 0;

UBaseType_t uxRand( void )
{
    const uint32_t ulMultiplier = 0x015a4e35UL, ulIncrement = 1UL;

    /*
     * Utility function to generate a pseudo random number.
     *
     * !!!NOTE!!!
     * This is not a secure method of generating a random number.  Production
     * devices should use a True Random Number Generator (TRNG).
     */
    ulNextRand = ( ulMultiplier * ulNextRand ) + ulIncrement;
    return( ( int ) ( ulNextRand >> 16UL ) & 0x7fffUL );
}

/*******************************************************************************
 * Code
 ******************************************************************************/

BaseType_t xApplicationGetRandomNumber( uint32_t *pulNumber )
{
	*pulNumber = uxRand();
	return pdTRUE;
}

/*!
 * @brief Application entry point.
 */
int main(void)
{
    /* Init board hardware. */
    CLOCK_EnableClock(kCLOCK_InputMux);

    /* attach 12 MHz clock to FLEXCOMM0 (debug console) */

    BOARD_InitBootPins();
    BOARD_InitBootClocks();
    BOARD_InitDebugConsole();

    FreeRTOS_IPInit( ucIPAddress, ucNetMask, ucGatewayAddress, ucDNSServerAddress, ucMACAddress );

    if (xTaskCreate(hello_task, "Hello_task", 1024, NULL, hello_task_PRIORITY, NULL) !=
        pdPASS)
    {
        PRINTF("Hello Task creation failed!.\r\n");
        while (1)
            ;
    }
    vTaskStartScheduler();
    for (;;)
        ;
}

/*!
 * @brief Task responsible for printing of "Hello world." message.
 */

uint32_t getTimeStampMs()
{
	return 0;
}

void eventCallback( MQTTContext_t * pContext, MQTTPacketInfo_t * pPacketInfo, MQTTDeserializedInfo_t * pDeserializedInfo );


static void hello_task(void *pvParameters)
{

	MQTTContext_t mqttContext;
	TransportInterface_t transport;
	MQTTFixedBuffer_t fixedBuffer;
	uint8_t buffer[ 1024 ];

	// Clear context.
	memset( ( void * ) &mqttContext, 0x00, sizeof( MQTTContext_t ) );

	// Set transport interface members.
	transport.pNetworkInterface = &someNetworkInterface;
	transport.send = Plaintext_FreeRTOS_send;
	transport.recv = Plaintext_FreeRTOS_recv;

	// Set buffer members.
	fixedBuffer.pBuffer = buffer;
	fixedBuffer.size = 1024;

	status = MQTT_Init( &mqttContext, &transport, getTimeStampMs, eventCallback, &fixedBuffer );

	if( status == MQTTSuccess )
	{
	    for (;;)
	    {
			// True for creating a new session with broker, false if we want to resume an old one.
			connectInfo.cleanSession = true;
			// Client ID must be unique to broker. This field is required.
			connectInfo.pClientIdentifier = "someClientID";
			connectInfo.clientIdentifierLength = strlen( connectInfo.pClientIdentifier );

			// The following fields are optional.
			// Value for keep alive.
			connectInfo.keepAliveSeconds = 60;
			// Optional username and password.
			connectInfo.pUserName = "mr_broker";
			connectInfo.userNameLength = strlen( connectInfo.pUserName );
			connectInfo.pPassword = "broker_password";
			connectInfo.passwordLength = strlen( connectInfo.pPassword );



			// Send the connect packet. Use 100 ms as the timeout to wait for the CONNACK packet.
			status = MQTT_Connect( &mqttCContext, &connectInfo, &willInfo, 100, &sessionPresent );
			if( status == MQTTSuccess )
			{
				// Since we requested a clean session, this must be false
				assert( sessionPresent == false );
				// Do something with the connection.



				MQTT_Disconnect(&mqttContext);
			}
	        vTaskDelay(pdMS_TO_TICKS(500));
	    }
	}

	for(;;)
	{
		PRINTF("MQTT FAILURE\r\n");
		vTaskDelay(pdMS_TO_TICKS(1000));
	}


}

void vApplicationIPNetworkEventHook( eIPCallbackEvent_t eNetworkEvent )
{
    uint32_t ulIPAddress, ulNetMask, ulGatewayAddress, ulDNSServerAddress;
    char cBuffer[ 16 ];
    static BaseType_t xTasksAlreadyCreated = pdFALSE;

    /* If the network has just come up...*/
    if( eNetworkEvent == eNetworkUp )
    {
        /* Create the tasks that use the IP stack if they have not already been
         * created. */
        if( xTasksAlreadyCreated == pdFALSE )
        {
            /* Demos that use the network are created after the network is
             * up. */
            PRINTF( ( "---------STARTING DEMO---------\r\n" ) );
            // vStartSimpleMQTTDemo();
            xTasksAlreadyCreated = pdTRUE;
        }

        /* Print out the network configuration, which may have come from a DHCP
         * server. */
        FreeRTOS_GetAddressConfiguration( &ulIPAddress, &ulNetMask, &ulGatewayAddress, &ulDNSServerAddress );
        FreeRTOS_inet_ntoa( ulIPAddress, cBuffer );
        PRINTF(  "\r\n\r\nIP Address: %s\r\n", cBuffer  );

        FreeRTOS_inet_ntoa( ulNetMask, cBuffer );
        PRINTF(  "Subnet Mask: %s\r\n", cBuffer  );

        FreeRTOS_inet_ntoa( ulGatewayAddress, cBuffer );
        PRINTF(  "Gateway Address: %s\r\n", cBuffer  );

        FreeRTOS_inet_ntoa( ulDNSServerAddress, cBuffer );
        PRINTF(  "DNS Server Address: %s\r\n\r\n\r\n", cBuffer  );
    }
}

extern uint32_t ulApplicationGetNextSequenceNumber( uint32_t ulSourceAddress,
                                                    uint16_t usSourcePort,
                                                    uint32_t ulDestinationAddress,
                                                    uint16_t usDestinationPort )
{
    ( void ) ulSourceAddress;
    ( void ) usSourcePort;
    ( void ) ulDestinationAddress;
    ( void ) usDestinationPort;

    return uxRand();
}

void vApplicationMallocFailedHook( void )
{
	for(;;);
}

