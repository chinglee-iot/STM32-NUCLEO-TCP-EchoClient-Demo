/* Standard includes. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Kernel includes. */
#include "FreeRTOS.h"
#include "task.h"

/* FreeRTOS+CLI includes. */
#include "FreeRTOS_CLI.h"

/*-----------------------------------------------------------*/

/**
 * @brief Interpreter that handles the trace command.
 */
static portBASE_TYPE prvTraceCommandInterpreter( char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString )
{
    const char * pcCommandParameter;
    BaseType_t xCommandParameterLength;

    configASSERT( pcWriteBuffer );

    pcCommandParameter = FreeRTOS_CLIGetParameter( pcCommandString, 1, &( xCommandParameterLength ) );

    if( pcCommandParameter != NULL )
    {
        if( strncmp( pcCommandParameter, "start", xCommandParameterLength ) == 0 )
        {
            snprintf( ( char * ) pcWriteBuffer, xWriteBufferLen, "OK" );
        }
        else if( strncmp( pcCommandParameter, "stop", xCommandParameterLength ) == 0 )
        {
            snprintf( ( char * ) pcWriteBuffer, xWriteBufferLen, "OK" );
        }
        else if( strncmp( pcCommandParameter, "get", xCommandParameterLength ) == 0 )
        {
            /* HACK - TRACE data is binary data and therefore, cannot be sent as
             * a null terminated string as payload can have legitimate null
             * bytes. Instead, we write a marker here in the output buffer which
             * the caller of FreeRTOS_CLIProcessCommand checks and sends the
             * actual data instead of this response. */
            snprintf( ( char * ) pcWriteBuffer, xWriteBufferLen, "TRACE-GET" );
        }
        else
        {
            snprintf( ( char * ) pcWriteBuffer, xWriteBufferLen, "Bad Command." );
        }
    }
    else
    {
        snprintf( ( char * ) pcWriteBuffer, xWriteBufferLen, "Bad Command." );
    }

    /* Return pdFALSE to indicate that the response is complete. */
    return pdFALSE;
}

/*-----------------------------------------------------------*/

/**
 * @brief Structure that defines the "trace" command line command.
 */
static const CLI_Command_Definition_t xTraceCommand =
{
    ( const char * const ) "trace", /* The command string to type. */
    ( const char * const ) "trace: Starts, stops and gets the trace according to the parameter - start/stop/get.\r\n",
    prvTraceCommandInterpreter, /* The interpreter function for the command. */
    1 /* One parameter - start, stop or get. */
};

/*-----------------------------------------------------------*/

void vRegisterTraceCommand( void )
{
    FreeRTOS_CLIRegisterCommand( &( xTraceCommand ) );
}

/*-----------------------------------------------------------*/

