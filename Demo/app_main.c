/* Standard includes. */
#include <stdio.h>
#include <stdlib.h>

/* Kernel includes. */
#include "FreeRTOS.h"
#include "task.h"

/* STM includes. */
#include "stm32h7xx_hal.h"

/* FreeRTOS+TCP includes. */
#include "FreeRTOS_Sockets.h"
#include "FreeRTOS_IP.h"

/* Logging includes. */
#include "logging.h"

/* CLI includes. */
#include "FreeRTOS_CLI.h"

/* Pcap capture includes. */
#include "pcap_capture.h"

/* Demo definitions. */
#define mainCLI_TASK_STACK_SIZE             512
#define mainCLI_TASK_PRIORITY               tskIDLE_PRIORITY

/* Logging module configuration. */
#define mainLOGGING_TASK_STACK_SIZE         256
#define mainLOGGING_TASK_PRIORITY           tskIDLE_PRIORITY
#define mainLOGGING_QUEUE_LENGTH            10
/*-----------------------------------------------------------*/

#include "pack_struct_start.h"


struct xPacketHeader
{
    uint8_t ucStartMarker;
    uint8_t ucPacketNumber;
    uint16_t usPayloadLength;
};



typedef struct xPacketHeader PacketHeader_t;

#define PACKET_HEADER_LENGTH    sizeof( PacketHeader_t )
#define PACKET_START_MARKER     0x55
/*-----------------------------------------------------------*/

extern UART_HandleTypeDef huart3;

const uint8_t ucMACAddress[ 6 ] = { configMAC_ADDR0, configMAC_ADDR1, configMAC_ADDR2, configMAC_ADDR3, configMAC_ADDR4, configMAC_ADDR5 };

static BaseType_t xTasksAlreadyCreated = pdFALSE;

extern RNG_HandleTypeDef hrng;

static char cInputCommandString[ configMAX_COMMAND_INPUT_SIZE + 1 ];
/*-----------------------------------------------------------*/

static void prvCliTask( void * pvParameters );

static void prvConfigureMPU( void );

static void prvRegisterCLICommands( void );

static BaseType_t prvIsValidRequest( const uint8_t * pucPacket, uint32_t ulPacketLength );

static BaseType_t prvSendCommandResponse( Socket_t xCLIServerSocket,
                                          struct freertos_sockaddr * pxSourceAddress,
                                          socklen_t xSourceAddressLength,
                                          const uint8_t * pucResponse,
                                          uint32_t ulResponseLength );
/*-----------------------------------------------------------*/

void app_main( void )
{
    BaseType_t xRet;
    const uint8_t ucIPAddress[ 4 ] = { configIP_ADDR0, configIP_ADDR1, configIP_ADDR2, configIP_ADDR3 };
    const uint8_t ucNetMask[ 4 ] = { configNET_MASK0, configNET_MASK1, configNET_MASK2, configNET_MASK3 };
    const uint8_t ucGatewayAddress[ 4 ] = { configGATEWAY_ADDR0, configGATEWAY_ADDR1, configGATEWAY_ADDR2, configGATEWAY_ADDR3 };
    const uint8_t ucDNSServerAddress[ 4 ] = { configDNS_SERVER_ADDR0, configDNS_SERVER_ADDR1, configDNS_SERVER_ADDR2, configDNS_SERVER_ADDR3 };

    prvConfigureMPU();

    /* Register all the commands with the FreeRTOS+CLI command
     * interpreter. */
    prvRegisterCLICommands();

    xRet = xLoggingTaskInitialize( mainLOGGING_TASK_STACK_SIZE,
                                   mainLOGGING_TASK_PRIORITY,
                                   mainLOGGING_QUEUE_LENGTH );
    configASSERT( xRet == pdPASS );

    configPRINTF( ( "Calling FreeRTOS_IPInit...\n" ) );
    FreeRTOS_IPInit( ucIPAddress, ucNetMask, ucGatewayAddress, ucDNSServerAddress, ucMACAddress );

    /* Start the RTOS scheduler. */
    vTaskStartScheduler();

    /* Infinite loop */
    for(;;)
    {
    }
}
/*-----------------------------------------------------------*/

static void prvCliTask( void *pvParameters )
{
    uint32_t ulResponseLength;
    BaseType_t xCount, xResponseRemaining, xResponseSent;
    Socket_t xCLIServerSocket = FREERTOS_INVALID_SOCKET;
    struct freertos_sockaddr xSourceAddress, xServerAddress;
    socklen_t xSourceAddressLength = sizeof( xSourceAddress );
    TickType_t xCLIServerRecvTimeout = portMAX_DELAY;
    char * pcOutputBuffer = FreeRTOS_CLIGetOutputBuffer();

    ( void ) pvParameters;

    xCLIServerSocket = FreeRTOS_socket( FREERTOS_AF_INET,
                                        FREERTOS_SOCK_DGRAM,
                                        FREERTOS_IPPROTO_UDP );
    configASSERT( xCLIServerSocket != FREERTOS_INVALID_SOCKET );

    /* No need to return from FreeRTOS_recvfrom until a message
     * is received. */
    FreeRTOS_setsockopt( xCLIServerSocket,
                         0,
                         FREERTOS_SO_RCVTIMEO,
                         &( xCLIServerRecvTimeout ),
                         sizeof( TickType_t ) );

    xServerAddress.sin_port = FreeRTOS_htons( configCLI_SERVER_PORT );
    xServerAddress.sin_addr = FreeRTOS_GetIPAddress();
    FreeRTOS_bind( xCLIServerSocket, &( xServerAddress ), sizeof( xServerAddress ) );

    configPRINTF( ( "Waiting for requests...\n" ) );

    for( ;; )
    {
        xCount = FreeRTOS_recvfrom( xCLIServerSocket,
                                    ( void * )( &( cInputCommandString[ 0 ] ) ),
                                    configMAX_COMMAND_INPUT_SIZE,
                                    0,
                                    &( xSourceAddress ),
                                    &( xSourceAddressLength ) );

        /* Since we set the receive timeout to portMAX_DELAY, the
         * above call should only return when a command is received. */
        configASSERT( xCount > 0 );
        cInputCommandString[ xCount ] = '\0';

        if( prvIsValidRequest( ( const uint8_t * ) &( cInputCommandString[ 0 ] ), xCount ) == pdTRUE )
        {
            configPRINTF( ( "Received command. IP:%x Port:%u Content:%s \n", xSourceAddress.sin_addr,
                                                                             xSourceAddress.sin_port,
                                                                             &( cInputCommandString[ PACKET_HEADER_LENGTH ] ) ) );

            /* Send the received command to the FreeRTOS+CLI. */
            xResponseRemaining = FreeRTOS_CLIProcessCommand( &( cInputCommandString[ PACKET_HEADER_LENGTH ] ),
                                                                pcOutputBuffer,
                                                                configCOMMAND_INT_MAX_OUTPUT_SIZE - 1 );

            configASSERT( xResponseRemaining == pdFALSE );

            /* Ensure null termination so that the strlen below does not
             * end up reading past bounds. */
            pcOutputBuffer[ configCOMMAND_INT_MAX_OUTPUT_SIZE - 1 ] = '\0';

            ulResponseLength = strlen( pcOutputBuffer );

            /* HACK - Check if the output buffer contains one of our special
             * markers indicating the need of a special response and process
             * accordingly. */
            if( strncmp( pcOutputBuffer, "PCAP-GET", ulResponseLength ) == 0 )
            {
                const uint8_t * pucPcapData;
                uint32_t ulPcapDataLength;

                pcap_capture_get_captured_data( &( pucPcapData ),
                                                &( ulPcapDataLength) );

                xResponseSent = prvSendCommandResponse( xCLIServerSocket,
                                                        &( xSourceAddress ),
                                                        xSourceAddressLength,
                                                        pucPcapData,
                                                        ulPcapDataLength );

                /* Next fetch should not get the same capture but the capture
                 * after this point. */
                pcap_capture_reset();
            }
            else
            {
                /* Send the command response. */
                xResponseSent = prvSendCommandResponse( xCLIServerSocket,
                                                        &( xSourceAddress ),
                                                        xSourceAddressLength,
                                                        ( const uint8_t * ) pcOutputBuffer,
                                                        ulResponseLength );
            }

            if( xResponseSent == pdPASS )
            {
                configPRINTF( ( "Response sent successfully. \n" ) );
            }
            else
            {
                configPRINTF( ( "[ERROR] Failed to send response. \n" ) );
            }
        }
        else
        {
            configPRINTF( ( "[ERROR] Malformed request. IP:%x Port:%u Content:%s \n", xSourceAddress.sin_addr,
                                                                                      xSourceAddress.sin_port,
                                                                                      cInputCommandString ) );
        }
    }
}
/*-----------------------------------------------------------*/

static void prvRegisterCLICommands( void )
{
extern void vRegisterPingCommand( void );
extern void vRegisterPcapCommand( void );
extern void vRegisterNetStatCommand( void );

    vRegisterPingCommand();
    vRegisterPcapCommand();
    vRegisterNetStatCommand();
}
/*-----------------------------------------------------------*/

static void prvConfigureMPU( void )
{
    MPU_Region_InitTypeDef MPU_InitStruct;

    HAL_MPU_Disable();

    /*  Configure the MPU attributes of RAM_D2 ( which contains .ethernet_data )
     * as Write back, Read allocate, Write allocate. */
    MPU_InitStruct.Enable           = MPU_REGION_ENABLE;
    MPU_InitStruct.BaseAddress      = 0x30000000;
    MPU_InitStruct.Size             = MPU_REGION_SIZE_256KB;
    MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
    MPU_InitStruct.IsBufferable     = MPU_ACCESS_NOT_BUFFERABLE;
    MPU_InitStruct.IsCacheable      = MPU_ACCESS_NOT_CACHEABLE;
    MPU_InitStruct.IsShareable      = MPU_ACCESS_SHAREABLE;
    MPU_InitStruct.Number           = MPU_REGION_NUMBER0;
    MPU_InitStruct.TypeExtField     = MPU_TEX_LEVEL1;
    MPU_InitStruct.SubRegionDisable = 0x00;
    MPU_InitStruct.DisableExec      = MPU_INSTRUCTION_ACCESS_ENABLE;

    HAL_MPU_ConfigRegion( &( MPU_InitStruct ) );

    HAL_MPU_Enable( MPU_PRIVILEGED_DEFAULT );
}
/*-----------------------------------------------------------*/

static BaseType_t prvIsValidRequest( const uint8_t * pucPacket, uint32_t ulPacketLength )
{
    BaseType_t xValidRequest = pdFALSE;
    PacketHeader_t * header;
    uint16_t usPayloadLength;

    if( ulPacketLength > PACKET_HEADER_LENGTH )
    {
        header = ( PacketHeader_t * )pucPacket;
        usPayloadLength = FreeRTOS_ntohs( header->usPayloadLength );

        if( ( header->ucStartMarker == PACKET_START_MARKER ) &&
            ( header->ucPacketNumber == 1 ) &&
            ( ( usPayloadLength + PACKET_HEADER_LENGTH ) == ulPacketLength ) )
        {
            xValidRequest = pdTRUE;
        }
    }

    return xValidRequest;
}
/*-----------------------------------------------------------*/

static BaseType_t prvSendCommandResponse( Socket_t xCLIServerSocket,
                                          struct freertos_sockaddr * pxSourceAddress,
                                          socklen_t xSourceAddressLength,
                                          const uint8_t * pucResponse,
                                          uint32_t ulResponseLength )
{
    BaseType_t ret = pdPASS;
    PacketHeader_t header;
    int32_t lBytesSent;
    uint8_t ucPacketNumber;
    uint32_t ulBytesToSend, ulRemainingBytes, ulBytesSent;

    ulRemainingBytes = ulResponseLength;
    ulBytesSent = 0;
    ucPacketNumber = 1;

    while( ulRemainingBytes > 0 )
    {
        ulBytesToSend = ulRemainingBytes;

        if( ulBytesToSend > ipMAX_UDP_PAYLOAD_LENGTH )
        {
            ulBytesToSend = ipMAX_UDP_PAYLOAD_LENGTH;
        }

        /* Send header. */
        header.ucStartMarker = PACKET_START_MARKER;
        header.ucPacketNumber = ucPacketNumber;
        ucPacketNumber++;
        header.usPayloadLength = FreeRTOS_htons( ( uint16_t ) ulBytesToSend );

        lBytesSent = FreeRTOS_sendto( xCLIServerSocket,
                                      ( const void * ) &( header ),
                                      sizeof( PacketHeader_t ),
                                      0,
                                      pxSourceAddress,
                                      xSourceAddressLength );

        if( lBytesSent != PACKET_HEADER_LENGTH )
        {
            configPRINTF( ("[ERROR] Failed to send response header.\n" ) );
            ret = pdFAIL;
            break;
        }

        /* Send actual response. */
        lBytesSent = FreeRTOS_sendto( xCLIServerSocket,
                                      ( const void * ) &( pucResponse[ ulBytesSent ] ),
                                      ulBytesToSend,
                                      0,
                                      pxSourceAddress,
                                      xSourceAddressLength );

        if( lBytesSent != ulBytesToSend )
        {
            configPRINTF( ("[ERROR] Failed to send response.\n" ) );
            ret = pdFAIL;
            break;
        }

        ulRemainingBytes -= ulBytesToSend;
        ulBytesSent += ulBytesToSend;
    }

    if( ret == pdPASS )
    {
        header.ucStartMarker = PACKET_START_MARKER;
        header.ucPacketNumber = ucPacketNumber;
        header.usPayloadLength = 0U;

        lBytesSent = FreeRTOS_sendto( xCLIServerSocket,
                                      ( const void * ) &( header ),
                                      sizeof( PacketHeader_t ),
                                      0,
                                      pxSourceAddress,
                                      xSourceAddressLength );

        if( lBytesSent != PACKET_HEADER_LENGTH )
        {
            configPRINTF( ("[ERROR] Failed to last response header.\n" ) );
            ret = pdFAIL;
        }
    }

    return ret;
}
/*-----------------------------------------------------------*/

uint32_t ulApplicationGetNextSequenceNumber( uint32_t ulSourceAddress,
                                             uint16_t usSourcePort,
                                             uint32_t ulDestinationAddress,
                                             uint16_t usDestinationPort )
{
    uint32_t ulReturn;

    ( void ) ulSourceAddress;
    ( void ) usSourcePort;
    ( void ) ulDestinationAddress;
    ( void ) usDestinationPort;

    xApplicationGetRandomNumber( &ulReturn );

    return ulReturn;
}
/*-----------------------------------------------------------*/

void vApplicationIPNetworkEventHook( eIPCallbackEvent_t eNetworkEvent )
{
    /* If the network has just come up...*/
    if( eNetworkEvent == eNetworkUp )
    {
    uint32_t ulIPAddress, ulNetMask, ulGatewayAddress, ulDNSServerAddress;
    char cBuffer[ 16 ];

        /* Create the tasks that use the IP stack if they have not already been
         * created. */
        if( xTasksAlreadyCreated == pdFALSE )
        {
            xTasksAlreadyCreated = pdTRUE;

            /* Sockets, and tasks that use the TCP/IP stack can be created here. */
            xTaskCreate( prvCliTask,
                         "cli",
                         mainCLI_TASK_STACK_SIZE,
                         NULL,
                         mainCLI_TASK_PRIORITY,
                         NULL );
        }

        /* Print out the network configuration, which may have come from a DHCP
         * server. */
        FreeRTOS_GetAddressConfiguration( &ulIPAddress, &ulNetMask, &ulGatewayAddress, &ulDNSServerAddress );
        FreeRTOS_inet_ntoa( ulIPAddress, cBuffer );
        configPRINTF( ( "IP Address: %s\n", cBuffer ) );

        FreeRTOS_inet_ntoa( ulNetMask, cBuffer );
        configPRINTF( ( "Subnet Mask: %s\n", cBuffer ) );

        FreeRTOS_inet_ntoa( ulGatewayAddress, cBuffer );
        configPRINTF( ( "Gateway Address: %s\n", cBuffer ) );

        FreeRTOS_inet_ntoa( ulDNSServerAddress, cBuffer );
        configPRINTF( ( "DNS Server Address: %s\n", cBuffer ) );
    }
}
/*-----------------------------------------------------------*/

BaseType_t xApplicationDNSQueryHook( const char *pcName )
{
    BaseType_t xReturn = pdFAIL;

    /* Determine if a name lookup is for this node.  Two names are given
     * to this node: that returned by pcApplicationHostnameHook() and that set
     * by mainDEVICE_NICK_NAME. */
    if( strcasecmp( pcName, pcApplicationHostnameHook() ) == 0 )
    {
        xReturn = pdPASS;
    }
    return xReturn;
}
/*-----------------------------------------------------------*/

const char *pcApplicationHostnameHook( void )
{
    /* Assign the name "STM32H7" to this network node.  This function will be
     * called during the DHCP: the machine will be registered with an IP address
     * plus this name. */
    return "STM32H7";
}
/*-----------------------------------------------------------*/

BaseType_t xApplicationGetRandomNumber( uint32_t *pulValue )
{
BaseType_t xReturn;

    if( HAL_RNG_GenerateRandomNumber( &hrng, pulValue ) == HAL_OK )
    {
        xReturn = pdPASS;
    }
    else
    {
        xReturn = pdFAIL;
    }

    return xReturn;
}
/*-----------------------------------------------------------*/

void vPrintStringToUart( const char *str )
{
    HAL_UART_Transmit( &( huart3 ), ( const uint8_t * )str, strlen( str ), 1000 );
}
/*-----------------------------------------------------------*/

void vApplicationStackOverflowHook( TaskHandle_t pxTask, char *pcTaskName )
{
    /* If configCHECK_FOR_STACK_OVERFLOW is set to either 1 or 2 then this
     * function will automatically get called if a task overflows its stack. */
    ( void ) pxTask;
    ( void ) pcTaskName;
    for( ;; );
}
/*-----------------------------------------------------------*/

void vApplicationMallocFailedHook( void )
{
    /* If configUSE_MALLOC_FAILED_HOOK is set to 1 then this function will
     * be called automatically if a call to pvPortMalloc() fails.  pvPortMalloc()
     * is called automatically when a task, queue or semaphore is created. */
    for( ;; );
}
/*-----------------------------------------------------------*/

/* configUSE_STATIC_ALLOCATION is set to 1, so the application must provide an
 * implementation of vApplicationGetIdleTaskMemory() to provide the memory that is
 * used by the Idle task. */
void vApplicationGetIdleTaskMemory( StaticTask_t **ppxIdleTaskTCBBuffer, StackType_t **ppxIdleTaskStackBuffer, uint32_t *pulIdleTaskStackSize )
{
/* If the buffers to be provided to the Idle task are declared inside this
 * function then they must be declared static - otherwise they will be allocated on
 * the stack and so not exists after this function exits. */
static StaticTask_t xIdleTaskTCB;
static StackType_t uxIdleTaskStack[ configMINIMAL_STACK_SIZE ];

    /* Pass out a pointer to the StaticTask_t structure in which the Idle task's
     * state will be stored. */
    *ppxIdleTaskTCBBuffer = &xIdleTaskTCB;

    /* Pass out the array that will be used as the Idle task's stack. */
    *ppxIdleTaskStackBuffer = uxIdleTaskStack;

    /* Pass out the size of the array pointed to by *ppxIdleTaskStackBuffer.
     * Note that, as the array is necessarily of type StackType_t,
     * configMINIMAL_STACK_SIZE is specified in words, not bytes. */
    *pulIdleTaskStackSize = configMINIMAL_STACK_SIZE;
}
/*-----------------------------------------------------------*/

/* configUSE_STATIC_ALLOCATION and configUSE_TIMERS are both set to 1, so the
 * application must provide an implementation of vApplicationGetTimerTaskMemory()
 * to provide the memory that is used by the Timer service task. */
void vApplicationGetTimerTaskMemory( StaticTask_t **ppxTimerTaskTCBBuffer, StackType_t **ppxTimerTaskStackBuffer, uint32_t *pulTimerTaskStackSize )
{
/* If the buffers to be provided to the Timer task are declared inside this
 * function then they must be declared static - otherwise they will be allocated on
 * the stack and so not exists after this function exits. */
static StaticTask_t xTimerTaskTCB;
static StackType_t uxTimerTaskStack[ configTIMER_TASK_STACK_DEPTH ];

    /* Pass out a pointer to the StaticTask_t structure in which the Timer
     * task's state will be stored. */
    *ppxTimerTaskTCBBuffer = &xTimerTaskTCB;

    /* Pass out the array that will be used as the Timer task's stack. */
    *ppxTimerTaskStackBuffer = uxTimerTaskStack;

    /* Pass out the size of the array pointed to by *ppxTimerTaskStackBuffer.
     * Note that, as the array is necessarily of type StackType_t,
     * configMINIMAL_STACK_SIZE is specified in words, not bytes. */
    *pulTimerTaskStackSize = configTIMER_TASK_STACK_DEPTH;
}
/*-----------------------------------------------------------*/
