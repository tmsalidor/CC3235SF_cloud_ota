/*
 * Copyright (c) 2016, Texas Instruments Incorporated
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * *  Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * *  Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * *  Neither the name of Texas Instruments Incorporated nor the names of
 *    its contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

//*****************************************************************************
//
//    Application Name     -   sample OTA application (nonos)
//    Application Overview -   This sample application demonstrates how to 
//                             start CC3x00 in WLAN-Station
//                             role and connect to a Wi-Fi access-point.
//                             The application
//                             connects to an AP and ping's the gateway. 
//                             It also checks for internet
//                             connectivity by pinging external host
//    Application Details  - Refer to 'Cloud OTA' README.html
//
//*****************************************************************************

/* Simplelink includes */
#include <ti/drivers/net/wifi/simplelink.h>
#include <ti/drivers/SPI.h>
#include "platform.h"
#include "appConfig.h"
#include <ti/net/ota/ota.h>
#include <ti/net/ota/otauser.h>
#include "cloud_ota.h"
#include "mqueue.h"

#ifndef NORTOS_SUPPORT
pthread_t g_ota_thread = (pthread_t)NULL;
pthread_t g_spawn_thread = (pthread_t)NULL;
#endif



#define OTA_DBG_PRINT UART_PRINT
/* continue is case of NO_UPDATE, OLDER_VERSION, 
  Init error without pending commit(< 20% ping) */
//#define OTA_LOOP_TESTING  
/* Don't wait for external trigger to start the OTA, 
run OTA after 5 ping sequences */
//#define DISABLE_OTA_SWITCH_TRIGGER    

/* local ota data */
OTA_memBlock otaMemBlock;
/* must be global, will be pointed by extlib_ota */
Ota_optServerInfo g_otaOptServerInfo; 

#define ASYNC_EVT_TIMEOUT   (10000)      /* In msecs */
#define LED_TOGGLE_TIMEOUT  (500)       /* In msecs */
#define PASS_PERCENTAGE     (10) //(80)

#define SL_STOP_TIMEOUT         300

#define OSI_STACK_SIZE          4048

#define PROVISIONING_APP_VERSION "1.0.16"
/* Provisioning inactivity timeout in seconds */
#define PROVISIONING_INACTIVITY_TIMEOUT                 600 
/* 15 seconds */    
#define RECONNECTION_ESTABLISHED_TIMEOUT                15000   

#if OTA_SERVER_TYPE == OTA_FILE_DOWNLOAD
	char g_OtaTarFileURL[] = "https://raw.githubusercontent.com/SimpleLink/CC3X20/master/SEC_OTA_CC3220SF/20190409153617_CC3220SF_cloud_ota.tar";
	#warning "g_OtaTarFileURL uses the URL of an example tarball, please update this global variable to use youe own setting."
#endif

const char *Roles[] = {"STA","STA","AP","P2P"};
const char *WlanStatus[] = {"DISCONNECTED","SCANING","CONNECTING","CONNECTED"};

uint8_t g_StopInProgress = 0;
uint8_t g_performOtaCommand = 0;

/*!
 *  \brief  Application's states
 */
typedef enum
{
    APP_STATE_STARTING,
    APP_STATE_WAIT_FOR_CONNECTION,
    APP_STATE_WAIT_FOR_IP,

    APP_STATE_PROVISIONING_IN_PROGRESS,
    APP_STATE_PROVISIONING_WAIT_COMPLETE,

    APP_STATE_PINGING_GW,

    APP_STATE_OTA_RUN,

    APP_STATE_ERROR,
    APP_STATE_MAX
}e_AppState;

/*!
 *  \brief  Application's events
 */
typedef enum
{
    APP_EVENT_NULL,
    APP_EVENT_STARTED,
    APP_EVENT_CONNECTED,
    APP_EVENT_IP_ACQUIRED,
    APP_EVENT_DISCONNECT,  /* used also for IP lost */
    APP_EVENT_PROVISIONING_STARTED,
    APP_EVENT_PROVISIONING_SUCCESS,
    APP_EVENT_PROVISIONING_STOPPED,
    APP_EVENT_PING_COMPLETE,
    APP_EVENT_OTA_START,
    APP_EVENT_CONTINUE,
    APP_EVENT_OTA_CHECK_DONE,
    APP_EVENT_OTA_DOWNLOAD_DONE,
    APP_EVENT_OTA_ERROR,
    APP_EVENT_TIMEOUT,
    APP_EVENT_ERROR,
    APP_EVENT_RESTART,
    APP_EVENT_MAX
}e_AppEvent;

/*
 *  \brief  Application state's context
 */
typedef struct
{
    e_AppState currentState;            /* Current state of the application */
    volatile uint32_t pendingEvents;    /* Events pending to be processed */
    uint8_t role;                       /* SimpleLink's role - STATION/AP/P2P */
    uint32_t asyncEvtTimeout;           /* Timeout value*/
    PlatformTimeout_t PlatformTimeout_Led;
}s_AppContext;

/*!
 *  \brief  Application data
 */
typedef struct
{
    SlNetAppPingReport_t pingReport;   /* Variable to store the ping report */
    uint32_t gatewayIP;                /* Variable to store the gateway IP
                                        * address
                                        */
}s_AppData;

/*!
 *  \brief  Function pointer to the event handler
 */
typedef int32_t (*fptr_EventHandler)();

/*!
 *  \brief  Entry in the lookup table
 */
typedef struct
{
    fptr_EventHandler p_evtHndl;    /* Pointer to the event handler */
    e_AppState nextState;           /* Next state of the application */
}s_TblEntry;

/*!
 *  \brief  Application state's context
 */
s_AppContext gAppCtx;

/*!
 *  \brief  Application's data
 */
s_AppData gAppData;

/* The message q */
mqd_t g_OtaSMQueue;

/*!
 *  \brief  This function displays the application banner
 *  \param  None
 *  \return None
 */
static void DisplayBanner();

/*!
 *   \brief      It starts the SimpleLink in the configured role. The device wil
 *              notify the host asynchronously when the initialization is
 *              complete
 *  \param[in]  role - Device shall be configured in this role
 *  \return     0 on success, -ve otherwise
 */
static int32_t InitSimplelink(uint8_t const role);

/*!
 *  \brief      Restarts the SimpleLink
 *  \param[in]  role - Device shall be configured in this role
 *  \return     0 on success, -ve otherwise
 */
static int32_t RestartSimplelink(uint8_t const role);

/*!
 *  \brief  This function toggles an LED
 *  \param  None
 *  \return 0 on success, -ve otherwise
 */
static int32_t ToggleLED();

/*!
 *  \brief  This function handles 'APP_EVENT_STARTED' event
 *  \param  None
 *  \return 0 on success, -ve otherwise
 */
static int32_t HandleStartConnect();

/*!
 *  \brief  This function configures Simplelink to ping the gateway IP and
 *          check the LAN connection
 *  \param  None
 *  \return 0 on success, -ve otherwise
 */
static int32_t CheckLanConnection();

/*!
 *  \brief  OTA access funtions
 *  \param  None
 *  \return 0 on success, -ve otherwise
 */
int32_t OtaInit();
int32_t OtaCheckAndDoCommit();
int32_t HandlePingComplete();
int32_t ProcessRestartMcu();
int32_t OtaImageTestingAndReset();
int32_t OtaRunStep();
int32_t OtaCount_Done = 0;
int32_t OtaCount_Warnings = 0;
int32_t OtaCount_Errors = 0;

/*!
 *  \brief  This function starts the async event timer
 *  \param  None
 *  \return 0 on success, -ve otherwise
 */
static int32_t StartAsyncEvtTimer();

/*!
 *  \brief  This function stops the async-event timer
 *  \param  None
 *  \return 0 on success, -ve otherwise
 */
static int32_t StopAsyncEvtTimer();

/*!
 *  \brief  This function reports the error condition by displaying the result
 *          on console o/p
 *  \param  None
 *  \return 0 on success, -ve otherwise
 */
static int32_t ReportError();

/*!
 *  \brief  This function restarts the SimpleLink on a fatal error
 *  \param  None
 *  \return 0 on success, -ve otherwise
 */
static int32_t ProcessRestartRequest();

/*!
 *  \brief  This function signals the application events
 *  \param  None
 *  \return 0 on success, -ve otherwise
 */
static int16_t SignalEvent(e_AppEvent event);

/* Provisioning Section */
int32_t ProvisioningExample();
int32_t HandleProvisioningComplete();
int32_t HandleProvisioningStop();

/*!
 *  \brief   Application lookup/transition table
 */
const s_TblEntry gTransitionTable[APP_STATE_MAX][APP_EVENT_MAX] =
{
    /* APP_STATE_STARTING */
    {
        /* APP_EVENT_NULL                   */ {ToggleLED, APP_STATE_STARTING},
        /* APP_EVENT_STARTED                */ {HandleStartConnect,
                                            APP_STATE_WAIT_FOR_CONNECTION    },
        /* APP_EVENT_CONNECTED              */ {StartAsyncEvtTimer,
                                                 APP_STATE_WAIT_FOR_IP       },
        /* APP_EVENT_IP_ACQUIRED            */ {ToggleLED /* AP-ignore*/,
                                                 APP_STATE_STARTING          },
        /* APP_EVENT_DISCONNECT             */ {ProcessRestartMcu,
                                                 APP_STATE_STARTING          },
        /* APP_EVENT_PROVISIONING_STARTED   */ {ReportError, APP_STATE_ERROR },
        /* APP_EVENT_PROVISIONING_SUCCESS   */ {ReportError, APP_STATE_ERROR },
        /* APP_EVENT_PROVISIONING_STOPPED   */ {ToggleLED,APP_STATE_STARTING },
        /* APP_EVENT_PING_COMPLETE          */ {ReportError, APP_STATE_ERROR },
        /* APP_EVENT_OTA_START              */ {ReportError, APP_STATE_ERROR },
        /* APP_EVENT_CONTINUE               */ {ReportError, APP_STATE_ERROR },
        /* APP_EVENT_OTA_CHECK_DONE         */ {ReportError, APP_STATE_ERROR },
        /* APP_EVENT_OTA_DOWNLOAD_DONE      */ {ReportError, APP_STATE_ERROR },
        /* APP_EVENT_TIMEOUT                */ {ReportError, APP_STATE_ERROR },
        /* APP_EVENT_ERROR                  */ {ReportError, APP_STATE_ERROR },
        /* APP_EVENT_RESTART                */ {ProcessRestartMcu,
                                                 APP_STATE_STARTING          }
    },

    /* APP_STATE_WAIT_FOR_CONNECTION */
    {
        /* APP_EVENT_NULL                    */ 
        {ToggleLED, APP_STATE_WAIT_FOR_CONNECTION     },
        /* APP_EVENT_STARTED                 */ 
        {HandleStartConnect, APP_STATE_WAIT_FOR_CONNECTION},
        /* APP_EVENT_CONNECTED               */ 
        {StartAsyncEvtTimer,APP_STATE_WAIT_FOR_IP     },
        /* APP_EVENT_IP_ACQUIRED             */ 
        {ReportError, APP_STATE_ERROR                  },
        /* APP_EVENT_DISCONNECT              */ 
        {ProcessRestartMcu,APP_STATE_STARTING         },
        /* APP_EVENT_PROVISIONING_STARTED    */ 
        {ProvisioningExample,APP_STATE_PROVISIONING_IN_PROGRESS },
        /* APP_EVENT_PROVISIONING_SUCCESS    */ 
        {ReportError, APP_STATE_ERROR                           },
        /* APP_EVENT_PROVISIONING_STOPPED    */ 
        {StartAsyncEvtTimer,APP_STATE_WAIT_FOR_CONNECTION       },
        /* APP_EVENT_PING_COMPLETE           */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_OTA_START               */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_CONTINUE                */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_OTA_CHECK_DONE          */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_OTA_DOWNLOAD_DONE       */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_OTA_ERROR               */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_TIMEOUT                 */ 
        {ProvisioningExample,APP_STATE_PROVISIONING_IN_PROGRESS  },
        /* APP_EVENT_ERROR                   */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_RESTART                 */ 
        {ProcessRestartMcu,APP_STATE_STARTING                    }
    },

    /* APP_STATE_WAIT_FOR_IP */
    {
        /* APP_EVENT_NULL                    */ 
        {ToggleLED,APP_STATE_WAIT_FOR_IP      },
        /* APP_EVENT_STARTED                 */ 
        {ProvisioningExample,APP_STATE_PROVISIONING_IN_PROGRESS },
        /* APP_EVENT_CONNECTED               */ 
        {ReportError, APP_STATE_ERROR                            },
        /* APP_EVENT_IP_ACQUIRED             */ 
        {CheckLanConnection,APP_STATE_PINGING_GW                   },
        /* APP_EVENT_DISCONNECT              */     
        /* OTA - on disconnection/ip lost do rollback by reset,
        no pending commit check */
        {ProcessRestartMcu,APP_STATE_STARTING                   },
        /* APP_EVENT_PROVISIONING_STARTED    */ 
        {ProvisioningExample,APP_STATE_PROVISIONING_IN_PROGRESS },
        /* APP_EVENT_PROVISIONING_SUCCESS    */ 
        {ToggleLED,APP_STATE_WAIT_FOR_IP      },
        /* APP_EVENT_PROVISIONING_STOPPED    */ 
        {HandleProvisioningComplete,APP_STATE_PINGING_GW        },
        /* APP_EVENT_PING_COMPLETE           */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_OTA_START               */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_CONTINUE                */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_OTA_CHECK_DONE          */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_OTA_DOWNLOAD_DONE       */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_OTA_ERROR               */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_TIMEOUT                 */ 
        {ProcessRestartMcu,APP_STATE_STARTING    },
        /* APP_EVENT_ERROR                   */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_RESTART                 */ 
        {ProcessRestartMcu,APP_STATE_STARTING    }
    },

    /* APP_STATE_PROVISIONING_IN_PROGRESS */
    {
        /* APP_EVENT_NULL                    */ 
        {ToggleLED, APP_STATE_PROVISIONING_IN_PROGRESS      },
        /* APP_EVENT_STARTED                 */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_CONNECTED               */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_IP_ACQUIRED             */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_DISCONNECT              */ 
        /* OTA - on disconnection/ip lost do rollback by reset,
        no pending commit check */
        {ProcessRestartMcu, APP_STATE_STARTING             },
        /* APP_EVENT_PROVISIONING_STARTED    */ 
        {ToggleLED,APP_STATE_PROVISIONING_IN_PROGRESS      },
        /* APP_EVENT_PROVISIONING_SUCCESS    */ 
        {HandleProvisioningComplete,APP_STATE_PROVISIONING_WAIT_COMPLETE},
        /* APP_EVENT_PROVISIONING_STOPPED    */ 
        {HandleProvisioningStop,APP_STATE_PINGING_GW       },
        /* APP_EVENT_PING_COMPLETE           */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_OTA_START               */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_CONTINUE                */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_OTA_CHECK_DONE          */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_OTA_DOWNLOAD_DONE       */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_OTA_ERROR               */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_TIMEOUT                 */ 
        {ProcessRestartMcu,APP_STATE_STARTING    },
        /* APP_EVENT_ERROR                   */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_RESTART                 */ 
        {ProcessRestartMcu,APP_STATE_STARTING    }
    },
    /* APP_STATE_PROVISIONING_WAIT_COMPLETE */
    {
        /* APP_EVENT_NULL                    */ 
        {ToggleLED,APP_STATE_PROVISIONING_WAIT_COMPLETE     },
        /* APP_EVENT_STARTED                 */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_CONNECTED               */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_IP_ACQUIRED             */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_DISCONNECT              */ 
        /* OTA - on disconnection/ip lost do rollback by reset, 
           no pending commit check */
        {ProcessRestartMcu,APP_STATE_STARTING     },                                
        /* APP_EVENT_PROVISIONING_STARTED    */ 
        {ToggleLED,APP_STATE_PROVISIONING_WAIT_COMPLETE      },
        /* APP_EVENT_PROVISIONING_SUCCESS    */ 
        {ToggleLED,APP_STATE_PROVISIONING_WAIT_COMPLETE      },
        /* APP_EVENT_PROVISIONING_STOPPED    */ 
        {HandleProvisioningStop,APP_STATE_PINGING_GW         },
        /* APP_EVENT_PING_COMPLETE           */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_OTA_START               */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_CONTINUE                */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_OTA_CHECK_DONE          */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_OTA_DOWNLOAD_DONE       */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_OTA_ERROR               */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_TIMEOUT                 */ 
        {ProcessRestartMcu,APP_STATE_STARTING    },
        /* APP_EVENT_ERROR                   */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_RESTART                 */ 
        {ProcessRestartMcu,APP_STATE_STARTING    }
    },

    /* APP_STATE_PINGING_GW */
    {
        /* APP_EVENT_NULL                    */ 
        {ToggleLED,APP_STATE_PINGING_GW       },
        /* APP_EVENT_STARTED                 */ 
        {ToggleLED,APP_STATE_PINGING_GW          },
        /* APP_EVENT_CONNECTED               */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_IP_ACQUIRED             */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_DISCONNECT              */ 
        /* OTA - on disconnection/ip lost do rollback by reset, 
           no pending commit check */
        {ProcessRestartMcu,APP_STATE_STARTING    }, 
        /* APP_EVENT_PROVISIONING_STARTED    */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_PROVISIONING_SUCCESS    */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_PROVISIONING_STOPPED    */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_PING_COMPLETE           */
        /* OTA - on ping complete, 
        check if commit the image and do another CheckLanConnection */
        {HandlePingComplete,APP_STATE_PINGING_GW }, 
        /* APP_EVENT_OTA_START               */ 
        /* OTA - Init OTA after bundle commit */
        {OtaInit, APP_STATE_OTA_RUN              }, 
        /* APP_EVENT_CONTINUE                */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_OTA_CHECK_DONE          */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_OTA_DOWNLOAD_DONE       */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_OTA_ERROR               */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_TIMEOUT                 */ 
        {ProcessRestartMcu,APP_STATE_STARTING    },
        /* APP_EVENT_ERROR                   */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_RESTART                 */ 
        {ProcessRestartMcu,APP_STATE_STARTING    }
    },

    /* APP_STATE_OTA_RUN */
    {
        /* APP_EVENT_NULL                    */ 
        {ToggleLED, APP_STATE_OTA_RUN            },
        /* APP_EVENT_STARTED                 */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_CONNECTED               */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_IP_ACQUIRED             */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_DISCONNECT              */ 
        /* OTA - on disconnection/ip lost do rollback by reset,
         no pending commit check */
        {ProcessRestartMcu,APP_STATE_STARTING    },                                 
        /* APP_EVENT_PROVISIONING_STARTED    */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_PROVISIONING_SUCCESS    */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_PROVISIONING_STOPPED    */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_PING_COMPLETE           */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_OTA_START               */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_CONTINUE                */ 
           /* OTA - run OTA steps untill download done */
        {OtaRunStep, APP_STATE_OTA_RUN           },                  
        /* APP_EVENT_OTA_CHECK_DONE          */ 
          /* OTA - back to pinging */
        {CheckLanConnection,APP_STATE_PINGING_GW },                               
        /* APP_EVENT_OTA_DOWNLOAD_DONE       */ 
        /* OTA - move bundle to testing mode and 
        reset the MCU/NWP and restart the SM */
        {OtaImageTestingAndReset,APP_STATE_STARTING    },                                 
        /* APP_EVENT_OTA_ERROR               */ 
         /* OTA - on ota error (security alert, 
         max consecutive retries, ...)  - stop */
        {ReportError, APP_STATE_ERROR            },                   
        /* APP_EVENT_TIMEOUT                 */ 
        {ReportError, APP_STATE_ERROR            },
        /* APP_EVENT_ERROR                   */ 
        /* OTA - sock error will produce APP_EVENT_RESTART event */
        {ReportError, APP_STATE_ERROR            },                    
        /* APP_EVENT_RESTART                 */ 
        {ProcessRestartMcu, APP_STATE_STARTING         }
    },

    /* APP_STATE_ERROR */
    {
        /* APP_EVENT_NULL                    */ {ReportError, APP_STATE_ERROR},
        /* APP_EVENT_STARTED                 */ {ReportError, APP_STATE_ERROR},
        /* APP_EVENT_CONNECTED               */ {ReportError, APP_STATE_ERROR},
        /* APP_EVENT_IP_ACQUIRED             */ {ReportError, APP_STATE_ERROR},
        /* APP_EVENT_DISCONNECT              */ {ReportError, APP_STATE_ERROR},
        /* APP_EVENT_PROVISIONING_STARTED    */ {ReportError, APP_STATE_ERROR},
        /* APP_EVENT_PROVISIONING_SUCCESS    */ {ReportError, APP_STATE_ERROR},
        /* APP_EVENT_PROVISIONING_STOPPED    */ {ReportError, APP_STATE_ERROR},
        /* APP_EVENT_PING_COMPLETE           */ {ReportError, APP_STATE_ERROR},
        /* APP_EVENT_OTA_START               */ {ReportError, APP_STATE_ERROR},
        /* APP_EVENT_CONTINUE                */ {ReportError, APP_STATE_ERROR},
        /* APP_EVENT_OTA_CHECK_DONE          */ {ReportError, APP_STATE_ERROR},
        /* APP_EVENT_OTA_DOWNLOAD_DONE       */ {ReportError, APP_STATE_ERROR},
        /* APP_EVENT_OTA_ERROR               */ {ReportError, APP_STATE_ERROR},
        /* APP_EVENT_TIMEOUT                 */ {ReportError, APP_STATE_ERROR},
        /* APP_EVENT_ERROR                   */ {ReportError, APP_STATE_ERROR},
        /* APP_EVENT_RESTART                 */ {ReportError, APP_STATE_ERROR}
    }
};

/* Provisioning Section */

int32_t HandleProvisioningStop()
{
    /* Get IP and Gateway information */
    uint16_t len = sizeof(SlNetCfgIpV4Args_t);
    uint16_t ConfigOpt = 0;   /* return value could be one of the following:
     SL_NETCFG_ADDR_DHCP / SL_NETCFG_ADDR_DHCP_LLA / SL_NETCFG_ADDR_STATIC  */
    SlNetCfgIpV4Args_t ipV4 = {0};

    sl_NetCfgGet(SL_NETCFG_IPV4_STA_ADDR_MODE,&ConfigOpt,&len,(uint8_t *)&ipV4);

    UART_PRINT(
        "\tDHCP is %s \r\n\tIP \t%d.%d.%d.%d \r\n\tMASK \t%d.%d.%d.%d \r\n\tGW"
        "\t%d.%d.%d.%d \r\n\tDNS \t%d.%d.%d.%d\r\n",
        (ConfigOpt == SL_NETCFG_ADDR_DHCP) ? "ON" : "OFF",
        SL_IPV4_BYTE(ipV4.Ip,3),
        SL_IPV4_BYTE(ipV4.Ip,2),
        SL_IPV4_BYTE(ipV4.Ip,1),
        SL_IPV4_BYTE(ipV4.Ip,0),
        SL_IPV4_BYTE(ipV4.IpMask,3),
        SL_IPV4_BYTE(ipV4.IpMask,2),
        SL_IPV4_BYTE(ipV4.IpMask,1),
        SL_IPV4_BYTE(ipV4.IpMask,0),
        SL_IPV4_BYTE(ipV4.IpGateway,3),
        SL_IPV4_BYTE(ipV4.IpGateway,2),
        SL_IPV4_BYTE(ipV4.IpGateway,1),
        SL_IPV4_BYTE(ipV4.IpGateway,0),
        SL_IPV4_BYTE(ipV4.IpDnsServer,3),
        SL_IPV4_BYTE(ipV4.IpDnsServer,2),
        SL_IPV4_BYTE(ipV4.IpDnsServer,1),
        SL_IPV4_BYTE(ipV4.IpDnsServer,0));

    /* This info is needed later on in the application. Saving it */
    gAppData.gatewayIP = ipV4.IpGateway;

    CheckLanConnection();

    return(0);
}

/*!
 *  \brief      Provisioning completed succesfully, connection established
 *              Open timer to make sure application nagotiation completed as well
 *  \param      None
 *  \return     None
 */
int32_t HandleProvisioningComplete()
{
    s_AppContext *const pCtx = &gAppCtx;

    UART_PRINT(
        "[Provisioning] Provisioning Application Ended Successfully \r\n ");
    pCtx->asyncEvtTimeout = RECONNECTION_ESTABLISHED_TIMEOUT;
    StartAsyncEvtTimer();

    return(0);
}

/*!
 *  \brief  Start Provisioning example
 *  \param
 *  \return none
 */
int32_t ProvisioningExample()
{
    int32_t retVal = 0;
    uint8_t configOpt = 0;
    uint16_t configLen = 0;
    SlDeviceVersion_t ver = {0};
    uint8_t simpleLinkMac[SL_MAC_ADDR_LEN];
    uint16_t macAddressLen;
    uint8_t provisioningCmd;

    UART_PRINT("\n\r\n\r\n\r==================================\n\r");
    UART_PRINT(" Provisioning Example Ver. %s\n\r",PROVISIONING_APP_VERSION);
    UART_PRINT("==================================\n\r");

    /* Get device's info */
    configOpt = SL_DEVICE_GENERAL_VERSION;
    configLen = sizeof(ver);
    retVal =
        sl_DeviceGet(SL_DEVICE_GENERAL, &configOpt, &configLen,
                     (uint8_t *)(&ver));

    if(SL_RET_CODE_PROVISIONING_IN_PROGRESS == retVal)
    {
        UART_PRINT(
            " [ERROR] Provisioning is already running,"
            " stopping current session...\r\n");
        SignalEvent(APP_EVENT_ERROR);
        return(0);
    }

    UART_PRINT(
        "\r\n CHIP 0x%x\r\n MAC  31.%d.%d.%d.%d\r\n PHY  %d.%d.%d.%d\r\n "
"NWP%d.%d.%d.%d\r\n ROM  %d\r\n HOST %d.%d.%d.%d\r\n",
        ver.ChipId,
        ver.FwVersion[0],
        ver.FwVersion[1],
        ver.FwVersion[2],
        ver.FwVersion[3],
        ver.PhyVersion[0],
        ver.PhyVersion[1],
        ver.PhyVersion[2],
        ver.PhyVersion[3],
        ver.NwpVersion[0],
        ver.NwpVersion[1],
        ver.NwpVersion[2],
        ver.NwpVersion[3],
        ver.RomVersion,
        SL_MAJOR_VERSION_NUM,SL_MINOR_VERSION_NUM,SL_VERSION_NUM,
        SL_SUB_VERSION_NUM);

    macAddressLen = sizeof(simpleLinkMac);
    sl_NetCfgGet(SL_NETCFG_MAC_ADDRESS_GET,NULL,&macAddressLen,
                 (unsigned char *)simpleLinkMac);
    UART_PRINT(" MAC address: %x:%x:%x:%x:%x:%x\r\n\r\n",
               simpleLinkMac[0],
               simpleLinkMac[1],
               simpleLinkMac[2],
               simpleLinkMac[3],
               simpleLinkMac[4],
               simpleLinkMac[5]);

    provisioningCmd = SL_WLAN_PROVISIONING_CMD_START_MODE_APSC;

    if(provisioningCmd <=
       SL_WLAN_PROVISIONING_CMD_START_MODE_APSC_EXTERNAL_CONFIGURATION)
    {
        UART_PRINT(
            "\r\n Starting Provisioning! mode=%d (0-AP, 1-SC, 2-AP+SC, "
            "3-AP+SC+WAC)\r\n\r\n",
            provisioningCmd);
    }
    else
    {
        UART_PRINT("\r\n Provisioning Command = %d \r\n\r\n",provisioningCmd);
    }

    /* start provisioning */
    retVal =
        sl_WlanProvisioning(provisioningCmd, ROLE_STA, 
        PROVISIONING_INACTIVITY_TIMEOUT,NULL,0);

    if(retVal < 0)
    {
        UART_PRINT(" Provisioning Command Error, num:%d\r\n",retVal);
    }

    return(0);
}

/*!
   \brief          This function handles general events
   \param[in]      pDevEvent - Pointer to structure containing general event info
   \return         None
 */
void SimpleLinkGeneralEventHandler(SlDeviceEvent_t *pDevEvent)
{
    if(NULL == pDevEvent)
    {
        return;
    }
    switch(pDevEvent->Id)
    {
    default:
    {
        if(pDevEvent->Data.Error.Code == SL_ERROR_LOADING_CERTIFICATE_STORE)
        {
            /* Ignore it */
            UART_PRINT(
                "GeneralEventHandler: EventId=%d,"
                "SL_ERROR_LOADING_CERTIFICATE_STORE, ErrorCode=%d\r\n",
                pDevEvent->Id, pDevEvent->Data.Error.Code);
            break;
        }
        UART_PRINT("Received unexpected General Event with code [0x%x]\r\n",
                   pDevEvent->Data.Error.Code);
        SignalEvent(APP_EVENT_ERROR);
    }
    break;
    }
}

/*
 *  \brief      This function handles WLAN async events
 *  \param[in]  pWlanEvent - Pointer to the structure containing WLAN event info
 *  \return     None
 */
void SimpleLinkWlanEventHandler(SlWlanEvent_t *pWlanEvent)
{
    SlWlanEventData_u *pWlanEventData = NULL;

    if(NULL == pWlanEvent)
    {
        return;
    }

    pWlanEventData = &pWlanEvent->Data;

    switch(pWlanEvent->Id)
    {
    case SL_WLAN_EVENT_CONNECT:
    {
        UART_PRINT("STA connected to AP %s, ",
                   pWlanEvent->Data.Connect.SsidName);

        UART_PRINT("BSSID is %02x:%02x:%02x:%02x:%02x:%02x\r\n",
                   pWlanEvent->Data.Connect.Bssid[0],
                   pWlanEvent->Data.Connect.Bssid[1],
                   pWlanEvent->Data.Connect.Bssid[2],
                   pWlanEvent->Data.Connect.Bssid[3],
                   pWlanEvent->Data.Connect.Bssid[4],
                   pWlanEvent->Data.Connect.Bssid[5]);

        SignalEvent(APP_EVENT_CONNECTED);
    }
    break;

    case SL_WLAN_EVENT_DISCONNECT:
    {
        SlWlanEventDisconnect_t *pDiscntEvtData = NULL;
        pDiscntEvtData = &pWlanEventData->Disconnect;

        /** If the user has initiated 'Disconnect' request, 'ReasonCode'
         * is SL_USER_INITIATED_DISCONNECTION
         */
        if(SL_WLAN_DISCONNECT_USER_INITIATED == pDiscntEvtData->ReasonCode)
        {
            UART_PRINT("Device disconnected from the AP on request\r\n");
        }
        else
        {
            UART_PRINT("Device disconnected from the AP on an ERROR\r\n");
        }

        SignalEvent(APP_EVENT_DISCONNECT);
    }
    break;

    case SL_WLAN_EVENT_PROVISIONING_PROFILE_ADDED:
        UART_PRINT(" [Provisioning] Profile Added: SSID: %s\r\n",
                   pWlanEvent->Data.ProvisioningProfileAdded.Ssid);
        if(pWlanEvent->Data.ProvisioningProfileAdded.ReservedLen > 0)
        {
            UART_PRINT(" [Provisioning] Profile Added: PrivateToken:%s\r\n",
                       pWlanEvent->Data.ProvisioningProfileAdded.Reserved);
        }
        break;

    case SL_WLAN_EVENT_PROVISIONING_STATUS:
    {
        switch(pWlanEvent->Data.ProvisioningStatus.ProvisioningStatus)
        {
        case SL_WLAN_PROVISIONING_GENERAL_ERROR:
        case SL_WLAN_PROVISIONING_ERROR_ABORT:
        case SL_WLAN_PROVISIONING_ERROR_ABORT_INVALID_PARAM:
        case SL_WLAN_PROVISIONING_ERROR_ABORT_HTTP_SERVER_DISABLED:
        case SL_WLAN_PROVISIONING_ERROR_ABORT_PROFILE_LIST_FULL:
        case SL_WLAN_PROVISIONING_ERROR_ABORT_PROVISIONING_ALREADY_STARTED:
            UART_PRINT(" [Provisioning] Provisioning Error status=%d\r\n",
                       pWlanEvent->Data.ProvisioningStatus.ProvisioningStatus);
            SignalEvent(APP_EVENT_ERROR);
            break;

        case SL_WLAN_PROVISIONING_CONFIRMATION_STATUS_FAIL_NETWORK_NOT_FOUND:
            UART_PRINT(
                " [Provisioning] Profile confirmation failed: network"
                "not found\r\n");
            SignalEvent(APP_EVENT_PROVISIONING_STARTED);
            break;

        case SL_WLAN_PROVISIONING_CONFIRMATION_STATUS_FAIL_CONNECTION_FAILED:
            UART_PRINT(
                " [Provisioning] Profile confirmation failed: Connection "
                "failed\r\n");
            SignalEvent(APP_EVENT_PROVISIONING_STARTED);
            break;

        case
            SL_WLAN_PROVISIONING_CONFIRMATION_STATUS_CONNECTION_SUCCESS_IP_NOT_ACQUIRED
            :
            UART_PRINT(
                " [Provisioning] Profile confirmation failed: IP address not "
                "acquired\r\n");
            SignalEvent(APP_EVENT_PROVISIONING_STARTED);
            break;

        case SL_WLAN_PROVISIONING_CONFIRMATION_STATUS_SUCCESS_FEEDBACK_FAILED:
            UART_PRINT(
                " [Provisioning] Profile Confirmation failed (Connection "
                "Success, feedback to Smartphone app failed)\r\n");
            SignalEvent(APP_EVENT_PROVISIONING_STARTED);
            break;

        case SL_WLAN_PROVISIONING_CONFIRMATION_STATUS_SUCCESS:
            UART_PRINT(" [Provisioning] Profile Confirmation Success!\r\n");
            SignalEvent(APP_EVENT_PROVISIONING_SUCCESS);
            break;

        case SL_WLAN_PROVISIONING_AUTO_STARTED:
            UART_PRINT(" [Provisioning] Auto-Provisioning Started\r\n");
            SignalEvent(APP_EVENT_PROVISIONING_STARTED);
            break;

        case SL_WLAN_PROVISIONING_STOPPED:
            UART_PRINT("\r\n Provisioning stopped:");
            UART_PRINT(" Current Role: %s\r\n",
                       Roles[pWlanEvent->Data.ProvisioningStatus.Role]);
            if(ROLE_STA == pWlanEvent->Data.ProvisioningStatus.Role)
            {
                UART_PRINT(
                    "                       WLAN Status: %s\r\n",
                    WlanStatus[pWlanEvent->Data.ProvisioningStatus.WlanStatus]);

                if(SL_WLAN_STATUS_CONNECTED ==
                   pWlanEvent->Data.ProvisioningStatus.WlanStatus)
                {
                    UART_PRINT(
                        "                       Connected to SSID: %s\r\n",
                        pWlanEvent->Data.ProvisioningStatus.Ssid);
                    SignalEvent(APP_EVENT_PROVISIONING_STOPPED);
                }
                else
                {
                    SignalEvent(APP_EVENT_PROVISIONING_STARTED);
                }
            }
            else
            {
                SignalEvent(APP_EVENT_PROVISIONING_STOPPED);
            }
            g_StopInProgress = 0;
            break;

        case SL_WLAN_PROVISIONING_SMART_CONFIG_SYNCED:
            UART_PRINT(" [Provisioning] Smart Config Synced!\r\n");
            break;

        case SL_WLAN_PROVISIONING_SMART_CONFIG_SYNC_TIMEOUT:
            UART_PRINT(" [Provisioning] Smart Config Sync Timeout!\r\n");
            break;

        case SL_WLAN_PROVISIONING_CONFIRMATION_WLAN_CONNECT:
            UART_PRINT(
                " [Provisioning] Profile confirmation: WLAN Connected!\r\n");
            break;

        case SL_WLAN_PROVISIONING_CONFIRMATION_IP_ACQUIRED:
            UART_PRINT(" [Provisioning] Profile confirmation: IP Acquired!\r\n");
            break;

        case SL_WLAN_PROVISIONING_EXTERNAL_CONFIGURATION_READY:
            UART_PRINT(" [Provisioning] External configuration is ready! \r\n");
            break;

        default:
            UART_PRINT(" [Provisioning] Unknown Provisioning Status: %d\r\n",
                       pWlanEvent->Data.ProvisioningStatus.ProvisioningStatus);
            break;
        }
    }
    break;

    default:
    {
        UART_PRINT("Unexpected WLAN event with Id [0x%x]\r\n", pWlanEvent->Id);
        SignalEvent(APP_EVENT_ERROR);
    }
    break;
    }
}

/*!
 *  \brief       The Function Handles the Fatal errors
 *  \param[in]  pFatalErrorEvent - Contains the fatal error data
 *  \return     None
 */
void SimpleLinkFatalErrorEventHandler(SlDeviceFatal_t *slFatalErrorEvent)
{
    switch(slFatalErrorEvent->Id)
    {
    case SL_DEVICE_EVENT_FATAL_DEVICE_ABORT:
    {
        UART_PRINT(
            "[ERROR] - FATAL ERROR: Abort NWP event detected: AbortType=%d,"
            "AbortData=0x%x\r\n",
            slFatalErrorEvent->Data.DeviceAssert.Code,
            slFatalErrorEvent->Data.DeviceAssert.Value);
    }
    break;

    case SL_DEVICE_EVENT_FATAL_DRIVER_ABORT:
    {
        UART_PRINT("[ERROR] - FATAL ERROR: Driver Abort detected. \r\n");
    }
    break;

    case SL_DEVICE_EVENT_FATAL_NO_CMD_ACK:
    {
        UART_PRINT(
            "[ERROR] - FATAL ERROR: No Cmd Ack detected [cmd opcode = 0x%x] \r\n",
            slFatalErrorEvent->Data.NoCmdAck.Code);
    }
    break;

    case SL_DEVICE_EVENT_FATAL_SYNC_LOSS:
    {
        UART_PRINT("[ERROR] - FATAL ERROR: Sync loss detected n\r");
        SignalEvent(APP_EVENT_RESTART);
        return;
    }

    case SL_DEVICE_EVENT_FATAL_CMD_TIMEOUT:
    {
        UART_PRINT(
            "[ERROR] - FATAL ERROR: Async event timeout detected "
            "[event opcode =0x%x]  \r\n",
            slFatalErrorEvent->Data.CmdTimeout.Code);
    }
    break;

    default:
        UART_PRINT("[ERROR] - FATAL ERROR: Unspecified error detected \r\n");
        break;
    }
    SignalEvent(APP_EVENT_ERROR);
}

/*!
 *  \brief      This function handles network events such as IP acquisition, IP
 *              leased, IP released etc.
 * \param[in]   pNetAppEvent - Pointer to the structure containing acquired IP
 * \return      None
 */
void SimpleLinkNetAppEventHandler(SlNetAppEvent_t *pNetAppEvent)
{
    SlNetAppEventData_u *pNetAppEventData = NULL;

    if(NULL == pNetAppEvent)
    {
        return;
    }

    pNetAppEventData = &pNetAppEvent->Data;

    switch(pNetAppEvent->Id)
    {
    case SL_NETAPP_EVENT_IPV4_ACQUIRED:
    {
        UART_PRINT("IPv4 acquired: IP = %d.%d.%d.%d\r\n", \
                   (uint8_t)SL_IPV4_BYTE(pNetAppEventData->IpAcquiredV4.Ip,3), \
                   (uint8_t)SL_IPV4_BYTE(pNetAppEventData->IpAcquiredV4.Ip,2), \
                   (uint8_t)SL_IPV4_BYTE(pNetAppEventData->IpAcquiredV4.Ip,1), \
                   (uint8_t)SL_IPV4_BYTE(pNetAppEventData->IpAcquiredV4.Ip,0));
        UART_PRINT("Gateway = %d.%d.%d.%d\r\n", \
                   (uint8_t)SL_IPV4_BYTE(pNetAppEventData->IpAcquiredV4.Gateway,
                                         3), \
                   (uint8_t)SL_IPV4_BYTE(pNetAppEventData->IpAcquiredV4.Gateway,
                                         2), \
                   (uint8_t)SL_IPV4_BYTE(pNetAppEventData->IpAcquiredV4.Gateway,
                                         1), \
                   (uint8_t)SL_IPV4_BYTE(pNetAppEventData->IpAcquiredV4.Gateway,
                                         0));

        /* This info is needed later on in the application. Saving it */
        gAppData.gatewayIP = pNetAppEventData->IpAcquiredV4.Gateway;

        SignalEvent(APP_EVENT_IP_ACQUIRED);
    }
    break;

    case SL_NETAPP_EVENT_IPV4_LOST:
    case SL_NETAPP_EVENT_DHCP_IPV4_ACQUIRE_TIMEOUT:
    {
        UART_PRINT("IPv4 lost Id or timeout, Id [0x%x]!!!\r\n",
                   pNetAppEvent->Id);
       /* use existing disconnect event, no need for another event */
        SignalEvent(APP_EVENT_DISCONNECT);     
    }
    break;

    default:
    {
        UART_PRINT("Unexpected NetApp event with Id [0x%x] \r\n",
                   pNetAppEvent->Id);
        SignalEvent(APP_EVENT_ERROR);
    }
    break;
    }
}

/*!
 *  \brief      This function handles ping init-complete event from SL
 *  \param[in]  status - Mode the device is configured in..!
 *  \param[in]  DeviceInitInfo - Device initialization information
 *  \return     None
 */
void SimpleLinkInitCallback(uint32_t status,
                            SlDeviceInitInfo_t *DeviceInitInfo)
{
    s_AppContext *const pCtx = &gAppCtx;

    if(pCtx->role == status)
    {
        switch(status)
        {
        case 0:
            UART_PRINT("Device started in Station role\r\n");
            break;

        case 1:
            UART_PRINT("Device started in P2P role\r\n");
            break;

        case 2:
            UART_PRINT("Device started in AP role\r\n");
            break;
        }

        UART_PRINT("Device Chip ID:   0x%08X\r\n", DeviceInitInfo->ChipId);
        UART_PRINT("Device More Data: 0x%08X\r\n", DeviceInitInfo->MoreData);

        SignalEvent(APP_EVENT_STARTED);
    }
    else
    {
        SignalEvent(APP_EVENT_ERROR);
    }
}

/*!
 *  \brief      This function handles ping report events
 *  \param[in]  pPingReport - Pointer to the structure containing ping report
 *  \return     None
 */
void SimpleLinkPingReport(SlNetAppPingReport_t *pPingReport)
{
    uint32_t successRate = 0;

    /* This info is needed later on in the application -  Saving it */
    memset(&gAppData.pingReport, 0, sizeof(SlNetAppPingReport_t));
    memcpy(&gAppData.pingReport, pPingReport, sizeof(SlNetAppPingReport_t));

    successRate = ((gAppData.pingReport.PacketsReceived * 100) / \
                   gAppData.pingReport.PacketsSent);
    UART_PRINT("Ping done. Success rate: %d%%", successRate);
    UART_PRINT("\r\n\r\n");

    if(successRate >= PASS_PERCENTAGE)
    {
        SignalEvent(APP_EVENT_PING_COMPLETE);
    }
    else
    {
        /* Restart MCU doing rollback if needed */
        SignalEvent(APP_EVENT_RESTART);
    }
}

/*!
 *  \brief      This function gets triggered when HTTP Server receives
 *              application defined GET and POST HTTP tokens.
 *  \param[in]  pHttpServerEvent Pointer indicating HTTP server event
 *  \param[in]  pHttpServerResponse Pointer indicating HTTP server response
 *  \return     None
 */
void SimpleLinkHttpServerEventHandler(
    SlNetAppHttpServerEvent_t *pHttpEvent,
    SlNetAppHttpServerResponse_t *
    pHttpResponse)
{
    /* Unused in this application */
    UART_PRINT("Unexpected HTTP server event \r\n");
    SignalEvent(APP_EVENT_ERROR);
}

/*!
 *  \brief      This function handles resource request
 *  \param[in]  pNetAppRequest - Contains the resource requests
 *  \param[in]  pNetAppResponse - Should be filled by the user with the
 *                                relevant response information
 *  \return     None
 */
void SimpleLinkNetAppRequestHandler(SlNetAppRequest_t  *pNetAppRequest,
                                    SlNetAppResponse_t *pNetAppResponse)
{
    /* Unused in this application */
    UART_PRINT("Unexpected NetApp request event \r\n");
    SignalEvent(APP_EVENT_ERROR);
}

/*!
 *  \brief      This function handles socket events indication
 *  \param[in]  pSock - Pointer to the structure containing socket event info
 *  \return     None
 */
void SimpleLinkSockEventHandler(SlSockEvent_t *pSock)
{
    if(pSock->Event == SL_SOCKET_TX_FAILED_EVENT)
    {
        /* on socket error Restart OTA */
        UART_PRINT("SL_SOCKET_TX_FAILED_EVENT socket event %d, do restart\r\n",
                   pSock->Event);
        SignalEvent(APP_EVENT_RESTART);
    }
    else if(pSock->Event == SL_SOCKET_ASYNC_EVENT)
    {
        /* on socket error Restart OTA */
        UART_PRINT("SL_SOCKET_ASYNC_EVENT socket event %d, do restart\r\n",
                   pSock->Event);
        SignalEvent(APP_EVENT_RESTART);
    }
    else
    {
        /* Unused in this application */
        UART_PRINT("Unexpected socket event %d\r\n", pSock->Event);
        SignalEvent(APP_EVENT_ERROR);
    }
}

#ifdef NORTOS_SUPPORT
void SimpleLinkSocketTriggerEventHandler(SlSockTriggerEvent_t *pSlTriggerEvent)
{
    /* Unused in this application */
}

#endif

/*!
 *  \brief      The interrupt handler for the async-event timer
 *  \param      None
 * \return     None
 */
void AsyncEvtTimerIntHandler(sigval val)
{
#ifdef USE_TIRTOS
    s_AppContext *const pCtx = &gAppCtx;
#endif
    Platform_TimerInterruptClear();
    /* One Shot timer */
    StopAsyncEvtTimer();
    SignalEvent(APP_EVENT_TIMEOUT);
}

static void DisplayBanner()
{
    UART_PRINT("\n\n\r");
    UART_PRINT(
        "*********************************************************************"
        "***********\n\r");
    UART_PRINT("\t          %s Application - Version %s        \n\r", \
               APPLICATION_NAME, APPLICATION_VERSION);
    UART_PRINT(
        "*********************************************************************"
        "***********\n\r");
    UART_PRINT("\n\r");
}

static int32_t InitSimplelink(uint8_t const role)
{
    s_AppContext *const pCtx = &gAppCtx;
    int32_t retVal = -1;
    int8_t event;
    s_TblEntry   *pEntry = NULL;

    pCtx->role = role;
    pCtx->currentState = APP_STATE_STARTING;
    pCtx->pendingEvents = 0;

    if((retVal = sl_Start(0, 0, 0)) == SL_ERROR_RESTORE_IMAGE_COMPLETE)
    {
        UART_PRINT("sl_Start Failed\r\n");
        UART_PRINT(
            "\r\n**********************************\r\nReturn to Factory "
            "Default been Completed\r\nPlease RESET the Board\r\n"
            "**********************************\r\n");
        Platform_FactoryDefaultIndication();
        while(1)
        {
            ;
        }
    }

    if(SL_RET_CODE_PROVISIONING_IN_PROGRESS == retVal)
    {
        UART_PRINT(
            " [ERROR] Provisioning is already running, stopping current "
            "session...\r\n");
        g_StopInProgress = 1;
        retVal = sl_WlanProvisioning(SL_WLAN_PROVISIONING_CMD_STOP,0, 0, NULL,
                                     0);
        while(g_StopInProgress)
        {
            mq_receive(g_OtaSMQueue, (char*)&event, 1, NULL);

            if(event != APP_EVENT_NULL)
            {
                StopAsyncEvtTimer();
            }

            /* Find Next event entry */
            pEntry = (s_TblEntry *)&gTransitionTable[pCtx->currentState][event];

            if(NULL != pEntry->p_evtHndl)
            {
                if(pEntry->p_evtHndl() < 0)
                {
                    UART_PRINT("Event handler failed..!! \r\n");
                    while(1)
                    {
                        ;
                    }
                }
            }

            /* Change state according to event */
            if(pEntry->nextState != pCtx->currentState)
            {
                pCtx->currentState = pEntry->nextState;
            }
        }

        retVal = sl_Start(0, 0, 0);
    }

    if(pCtx->role == retVal)
    {
        UART_PRINT("SimpleLinkInitCallback: started in role %d\r\n", pCtx->role);
        SignalEvent(APP_EVENT_STARTED);
    }
    else
    {
        UART_PRINT(
            "SimpleLinkInitCallback: started in role %d, set the requested "
            "role %d\r\n",
            retVal, pCtx->role);
        retVal = sl_WlanSetMode(pCtx->role);
        ASSERT_ON_ERROR(retVal);
        retVal = sl_Stop(SL_STOP_TIMEOUT);
        ASSERT_ON_ERROR(retVal);
        retVal = sl_Start(0, 0, 0);
        ASSERT_ON_ERROR(retVal);
        if(pCtx->role != retVal)
        {
            UART_PRINT(
                "SimpleLinkInitCallback: error setting role %d, status=%d\r\n",
                pCtx->role, retVal);
            SignalEvent(APP_EVENT_ERROR);
        }
        UART_PRINT("SimpleLinkInitCallback: restarted in role %d\r\n",
                   pCtx->role);
        pCtx->pendingEvents = 0;
        SignalEvent(APP_EVENT_STARTED);
    }

    /* Start timer */
    pCtx->asyncEvtTimeout = ASYNC_EVT_TIMEOUT;
    retVal = StartAsyncEvtTimer();
    ASSERT_ON_ERROR(retVal);

    return(retVal);
}

static int32_t RestartSimplelink(uint8_t const role)
{
    int32_t retVal = -1;

    retVal = sl_Stop(SL_STOP_TIMEOUT);
    ASSERT_ON_ERROR(retVal);

    retVal = InitSimplelink(role);
    ASSERT_ON_ERROR(retVal);

    return(retVal);
}

static int32_t ToggleLED()
{
    s_AppContext *const pCtx = &gAppCtx;

    if(Platform_TimeoutIsExpired(&pCtx->PlatformTimeout_Led))
    {
        Platform_LedToggle();
        Platform_TimeoutStart(&pCtx->PlatformTimeout_Led, LED_TOGGLE_TIMEOUT);
    }

    return(0);
}

static int32_t HandleStartConnect()
{
    SlDeviceVersion_t firmwareVersion = {0};
    s_AppContext *const pCtx = &gAppCtx;
    int8_t event;
    s_TblEntry   *pEntry = NULL;

    int32_t retVal = -1;

    uint8_t ucConfigOpt = 0;
    uint16_t ucConfigLen = 0;

    /* Get the device's version-information */
    ucConfigOpt = SL_DEVICE_GENERAL_VERSION;
    ucConfigLen = sizeof(firmwareVersion);
    retVal =
        sl_DeviceGet(SL_DEVICE_GENERAL, &ucConfigOpt, &ucConfigLen,
                     (uint8_t *)(&firmwareVersion));
    if(SL_RET_CODE_PROVISIONING_IN_PROGRESS == retVal)
    {
        UART_PRINT(
            " [ERROR] Provisioning is already running, stopping "
            "current session...\r\n");
        g_StopInProgress = 1;
        retVal = sl_WlanProvisioning(SL_WLAN_PROVISIONING_CMD_STOP,0, 0, NULL,
                                     0);

        while(g_StopInProgress)
        {
            mq_receive(g_OtaSMQueue, (char*)&event, 1, NULL);

            if(event != APP_EVENT_NULL)
            {
                StopAsyncEvtTimer();
            }

            /* Find Next event entry */
            pEntry = (s_TblEntry *)&gTransitionTable[pCtx->currentState][event];

            if(NULL != pEntry->p_evtHndl)
            {
                if(pEntry->p_evtHndl() < 0)
                {
                    UART_PRINT("Event handler failed..!! \r\n");
                    while(1)
                    {
                        ;
                    }
                }
            }

            /* Change state according to event */
            if(pEntry->nextState != pCtx->currentState)
            {
                pCtx->currentState = pEntry->nextState;
            }
        }
    }
    ASSERT_ON_ERROR(retVal);

    UART_PRINT("Host Driver Version: %s\r\n", SL_DRIVER_VERSION);
    UART_PRINT("Build Version %d.%d.%d.%d.31.%d.%d.%d.%d.%d.%d.%d.%d\r\n", \
               firmwareVersion.NwpVersion[0], firmwareVersion.NwpVersion[1], \
               firmwareVersion.NwpVersion[2], firmwareVersion.NwpVersion[3], \
               firmwareVersion.FwVersion[0], firmwareVersion.FwVersion[1], \
               firmwareVersion.FwVersion[2], firmwareVersion.FwVersion[3], \
               firmwareVersion.PhyVersion[0], firmwareVersion.PhyVersion[1], \
               firmwareVersion.PhyVersion[2], firmwareVersion.PhyVersion[3]);

    pCtx->asyncEvtTimeout = ASYNC_EVT_TIMEOUT;
    StartAsyncEvtTimer();

    return(retVal);
}

#ifdef TIMER_NOT_IMPLEMENTED
/**/
static int32_t WaitForConnection()
{
    s_AppContext *const pCtx = &gAppCtx;
    int32_t sleepCnt;

    /* Wait for wlan connection */
    sleepCnt = 0;
    while((pCtx->pendingEvents & (1 << APP_EVENT_CONNECTED)) == 0)
    {
        sl_Task();
        Platform_Sleep(20);
        if(++sleepCnt >= 500)
        {
            UART_PRINT(("sl_WlanConnect timeout\r\n"));
            SignalEvent(APP_EVENT_TIMEOUT);
            return(0);
        }
    }

    /* Wait for IP */
    sleepCnt = 0;
    while((pCtx->pendingEvents & (1 << APP_EVENT_IP_ACQUIRED)) == 0)
    {
        sl_Task();
        Platform_Sleep(20);
        if(++sleepCnt >= 500)
        {
            UART_PRINT(("net connection timeout\r\n"));
            SignalEvent(APP_EVENT_TIMEOUT);
            return(0);
        }
    }
    return(0);
}

#endif

static int32_t CheckLanConnection()
{
    s_AppContext *const pCtx = &gAppCtx;

    SlNetAppPingCommand_t pingParams = {0};
    SlNetAppPingReport_t pingReport = {0};

    int32_t retVal = -1;

    /* Set the ping parameters */
    pingParams.PingIntervalTime = PING_INTERVAL;
    pingParams.PingSize = PING_PKT_SIZE;
    pingParams.PingRequestTimeout = PING_TIMEOUT;
    pingParams.TotalNumberOfAttempts = NO_OF_ATTEMPTS;
    pingParams.Flags = 0;

    pingParams.Ip = gAppData.gatewayIP;

    /* Ping the GW */
    retVal = sl_NetAppPing((SlNetAppPingCommand_t*)&pingParams, \
                           SL_AF_INET, (SlNetAppPingReport_t*)&pingReport, \
                           SimpleLinkPingReport);
    ASSERT_ON_ERROR(retVal);

    /* Compute worst case ping timeout */
    pCtx->asyncEvtTimeout = ((NO_OF_ATTEMPTS * PING_TIMEOUT) + \
                             (NO_OF_ATTEMPTS * PING_INTERVAL) + \
                             (2000)); /* Additional time */
    retVal = StartAsyncEvtTimer();
    ASSERT_ON_ERROR(retVal);

    UART_PRINT("Pinging GW...!\r\n");

    return(retVal);
}

static int32_t StartAsyncEvtTimer()
{
    s_AppContext *const pCtx = &gAppCtx;

    if(0 == pCtx->asyncEvtTimeout)
    {
        pCtx->asyncEvtTimeout = ASYNC_EVT_TIMEOUT;
    }

    Platform_TimerStart(pCtx->asyncEvtTimeout);
    return(0);
}

static int32_t StopAsyncEvtTimer()
{
    s_AppContext *const pCtx = &gAppCtx;

    if(0 != pCtx->asyncEvtTimeout)
    {
        Platform_TimerStop();
        pCtx->asyncEvtTimeout = 0;
    }

    return(0);
}

static int32_t ReportError()
{
    s_AppContext *const pCtx = &gAppCtx;
    uint16_t eventIdx = 0;

    for(eventIdx = 0; eventIdx < APP_EVENT_MAX; eventIdx++)
    {
        if(0 != (pCtx->pendingEvents & (1 << eventIdx)))
        {
            break;
        }
    }

    UART_PRINT("Test failed: State = %d, Event = %d\r\n", pCtx->currentState,
               eventIdx);
    return(-1); /**/
}

static int32_t ProcessRestartRequest()
{
    s_AppContext *const pCtx = &gAppCtx;
    int32_t retVal = -1;

    retVal = RestartSimplelink(pCtx->role);
    ASSERT_ON_ERROR(retVal);

    return(retVal);
}

int32_t OtaInit()
{
    int16_t Status;

    StopAsyncEvtTimer();

    /* Configure the commit watchdog timer in seconds */
    Status = Platform_CommitWdtConfig(50);
    if(Status < 0)
    {
        OTA_DBG_PRINT(
            "OtaInit: ERROR from Platform_CommitWdtConfig. Status=%d\r\n",
            Status);
        SignalEvent(APP_EVENT_ERROR);
        return(Status);
    }

    OTA_DBG_PRINT("OtaInit: statistics = %d, %d, %d\r\n", OtaCount_Done,
                  OtaCount_Warnings,
                  OtaCount_Errors);
    /* init OTA */
    OTA_DBG_PRINT("OtaInit: call Ota_init\r\n");
    Status = OTA_init(OTA_RUN_NON_BLOCKING, &otaMemBlock, NULL);
    if(Status < 0)
    {
        OTA_DBG_PRINT("OtaInit: ERROR from Ota_init. Status=%d\r\n", Status);
        SignalEvent(APP_EVENT_ERROR); /* Fatal error */
        return(Status);
    }

    /* set OTA server info */
#if OTA_SERVER_TYPE == OTA_FILE_DOWNLOAD
	Status = OTA_set(EXTLIB_OTA_SET_OPT_FILE_SERVER_URL, sizeof(g_OtaTarFileURL), (uint8_t *)g_OtaTarFileURL, 0);
    if (Status < 0)
    {
        OTA_DBG_PRINT("OtaInit: ERROR from OTA_set EXTLIB_OTA_SET_OPT_SERVER_INFO. Status=%d\r\n", Status);
        SignalEvent(APP_EVENT_ERROR); /* Fatal error */
        return Status;
    }
#else
    OTA_DBG_PRINT(
        "OtaConfig: call OTA_set EXTLIB_OTA_SET_OPT_SERVER_INFO,"
        "ServerName=%s\r\n",
        OTA_SERVER_NAME);
    g_otaOptServerInfo.IpAddress = OTA_SERVER_IP_ADDRESS;
    g_otaOptServerInfo.SecuredConnection = OTA_SERVER_SECURED;
    strcpy((char *)g_otaOptServerInfo.ServerName,  OTA_SERVER_NAME);
    strcpy((char *)g_otaOptServerInfo.VendorToken, OTA_VENDOR_TOKEN);
    Status =
        OTA_set(EXTLIB_OTA_SET_OPT_SERVER_INFO, sizeof(g_otaOptServerInfo),
                (uint8_t *)&g_otaOptServerInfo, 0);
    if(Status < 0)
    {
        OTA_DBG_PRINT(
            "OtaInit: ERROR from OTA_set EXTLIB_OTA_SET_OPT_SERVER_INFO."
            "Status=%d\r\n",
            Status);
        SignalEvent(APP_EVENT_ERROR); /* Fatal error */
        return(Status);
    }

    /* set vendor ID */
    OTA_DBG_PRINT(
        "OtaConfig: call OTA_set EXTLIB_OTA_SET_OPT_VENDOR_ID, "
        "VendorDir=%s\r\n",
        OTA_VENDOR_DIR);
    Status =
        OTA_set(EXTLIB_OTA_SET_OPT_VENDOR_ID, strlen(
                    OTA_VENDOR_DIR), (uint8_t *)OTA_VENDOR_DIR, 0);
    if(Status < 0)
    {
        OTA_DBG_PRINT(
            "OtaInit: ERROR from OTA_set EXTLIB_OTA_SET_OPT_VENDOR_ID."
            "Status=%d\r\n",
            Status);
        SignalEvent(APP_EVENT_ERROR); /* Fatal error */
        return(Status);
    }
#endif
    SignalEvent(APP_EVENT_CONTINUE);
    return(Status);
}

int32_t OtaCheckAndDoCommit()
{
    int32_t isPendingCommit;
    int32_t isPendingCommit_len;
    int32_t Status;

    /* At this stage we have fully connected to the network (IPv4_Acquired) */
    /* If the MCU image is under test, the ImageCommit process will 
    commit the new image and might reset the MCU */
    Status =
        OTA_get(EXTLIB_OTA_GET_OPT_IS_PENDING_COMMIT,
                (int32_t *)&isPendingCommit_len,
                (uint8_t *)&isPendingCommit);
    if(Status < 0)
    {
        OTA_DBG_PRINT(
            "OtaCheckDoCommit: OTA_get ERROR on "
            "EXTLIB_OTA_GET_OPT_IS_PENDING_COMMIT, Status = %d\r\n",
            Status);
        SignalEvent(APP_EVENT_ERROR); /* Fatal error */
        return(0); /* action in state machine - return 0 in order to process the
        APP_EVENT_ERROR */
    }

    /* commit now because 1. the state is PENDING_COMMIT 2. there was successful
    wlan connection */
    if(isPendingCommit)
    {
        Status = OTA_set(EXTLIB_OTA_SET_OPT_IMAGE_COMMIT, 0, NULL, 0);
        if(Status < 0)
        {
            OTA_DBG_PRINT(
                "OtaCheckDoCommit: OTA_set ERROR on "
                "EXTLIB_OTA_SET_OPT_IMAGE_COMMIT, Status = %d\r\n",
                Status);
            SignalEvent(APP_EVENT_ERROR); /* Can be in wrong state,
            ToDo - no error */
            return(0); /* action in state machine -
            return 0 in order to process the APP_EVENT_ERROR */
        }
        OTA_DBG_PRINT("\r\n");
        OTA_DBG_PRINT(
            "OtaCheckDoCommit: OTA success, new image commited and "
            "currently run\n");
        OTA_DBG_PRINT("\r\n");
        /* Stop the commit WDT */
        Platform_CommitWdtStop();
    }
    return(0);
}

/*!
 *  \brief  External indication (switch) to perform OTA
 *  \param  None
 *  \return none
 */
void notifyOtaCommandArrived()
{
    g_performOtaCommand = 1;
}

#define OTA_PERIOD   5 /* number of pings check */
int32_t PingCounter = 0;
int32_t HandlePingComplete()
{
    OtaCheckAndDoCommit();
    PingCounter++;

#ifndef DISABLE_OTA_SWITCH_TRIGGER
    if(g_performOtaCommand)
#else
    /* Don't wait for external trigger to start the OTA,
run OTA after 5 ping sequences */
    if(PingCounter >= OTA_PERIOD)
#endif
    {
        /* It's time to start periodic OTA check */
        OTA_DBG_PRINT("HandlePingComplete: OTA Command arrived\r\n");
        PingCounter = 0;
        g_performOtaCommand = 0;
        SignalEvent(APP_EVENT_OTA_START);
    }
    else
    {
        /* Continue pinging */
        OTA_DBG_PRINT("HandlePingComplete: PingCounter=%d\r\n", PingCounter);
        CheckLanConnection();
    }
    return(0);
}

int32_t ProcessRestartMcu()
{
    OTA_DBG_PRINT("\n\n");
    OTA_DBG_PRINT("ProcessRestartMcu: reset the platform...\r\n");
    Platform_Reset();

    /* if we reach here, the platform does not support self reset */
    /* reset the NWP in order to rollback to the old image */
    OTA_DBG_PRINT("\n");
    OTA_DBG_PRINT("ProcessRestartMcu: platform does not support "
    "self reset\r\n");
    OTA_DBG_PRINT(
        "ProcessRestartMcu: reset the NWP to rollback to the old image\r\n");
    OTA_DBG_PRINT("\n");
    ProcessRestartRequest();       /* sl_Stop and sl_Start */
    return(0);
}

int32_t OtaRunStep()
{
    int32_t Status;
    Ota_optVersionsInfo VersionsInfo;
    int32_t Optionlen;

    Status = OTA_run();
    switch(Status)
    {
    case OTA_RUN_STATUS_CONTINUE:
        /* continue calling Ota_run */
        SignalEvent(APP_EVENT_CONTINUE);
        break;

    case OTA_RUN_STATUS_CONTINUE_WARNING_FAILED_CONNECT_OTA_SERVER:
    case OTA_RUN_STATUS_CONTINUE_WARNING_FAILED_RECV_APPEND:
    case OTA_RUN_STATUS_CONTINUE_WARNING_FAILED_REQ_OTA_DIR:
    case OTA_RUN_STATUS_CONTINUE_WARNING_FAILED_REQ_FILE_URL:
    case OTA_RUN_STATUS_CONTINUE_WARNING_FAILED_CONNECT_FILE_SERVER:
    case OTA_RUN_STATUS_CONTINUE_WARNING_FAILED_REQ_FILE_CONTENT:
    case OTA_RUN_STATUS_CONTINUE_WARNING_FAILED_FILE_HDR:
    case OTA_RUN_STATUS_CONTINUE_WARNING_FAILED_DOWNLOAD_AND_SAVE:
        OTA_DBG_PRINT(
            "OtaRunStep: WARNING Ota_run, Status=%d, continue for"
            "next OTA retry\r\n",
            Status);
        OTA_DBG_PRINT("\r\n");
        OtaCount_Warnings++;
        /* on warning, continue calling Ota_run for next retry */
        SignalEvent(APP_EVENT_CONTINUE);
        break;

    case OTA_RUN_STATUS_NO_UPDATES:
        /* OTA will go back to IDLE and next Ota_run will restart the process */
        OTA_DBG_PRINT("\r\n");
        OTA_DBG_PRINT("OtaRunStep: status from Ota_run: no updates\r\n");
        OTA_DBG_PRINT("\r\n");
        OTA_set(EXTLIB_OTA_SET_OPT_DECLINE_UPDATE, 0, NULL, 0);
        SignalEvent(APP_EVENT_OTA_CHECK_DONE);
        break;

    case OTA_RUN_STATUS_CHECK_NEWER_VERSION:
        /* OTA find new version - in compare to ota.dat file version */
        /* host should decide if to use the new version or to ignore it */
        OTA_DBG_PRINT(
            "OtaRunStep: status from Ota_run: "
            "OTA_RUN_STATUS_CHECK_NEWER_VERSION, accept and continue\r\n");
        OTA_get(EXTLIB_OTA_GET_OPT_VERSIONS, (int32_t *)&Optionlen,
                (uint8_t *)&VersionsInfo);
        OTA_DBG_PRINT(
            "OtaRunStep: CurrentVersion=%s, NewVersion=%s,"
            " Start download ...\r\n",
            VersionsInfo.CurrentVersion, VersionsInfo.NewVersion);

        OTA_set(EXTLIB_OTA_SET_OPT_ACCEPT_UPDATE, 0, NULL, 0);
        SignalEvent(APP_EVENT_CONTINUE);
        break;

    case OTA_RUN_STATUS_CHECK_OLDER_VERSION:
        /* OTA find old version - in compare to ota.dat file version */
        /* host should decide if to use this old version or to ignore it */
        Optionlen = sizeof(Ota_optVersionsInfo);
        OTA_DBG_PRINT("\r\n");
        OTA_DBG_PRINT(
            "OtaRunStep: status from Ota_run: "
            "OTA_RUN_STATUS_CHECK_OLDER_VERSION \r\n");
        OTA_DBG_PRINT("\r\n");
#ifdef OTA_LOOP_TESTING
        /* just for loop testing  - ignore status OLDER_VERSION */
        OTA_DBG_PRINT("OtaRunStep: ignore it just for loop testing\r\n");
        OTA_set(EXTLIB_OTA_SET_OPT_ACCEPT_UPDATE, 0, NULL, 0);
#endif
        SignalEvent(APP_EVENT_CONTINUE);
       break;


    case OTA_RUN_STATUS_DOWNLOAD_DONE:
        OTA_DBG_PRINT(
            "OtaRunStep: status from Ota_run: Download done, status = %d\r\n",
            Status);
        OtaCount_Done++;
        SignalEvent(APP_EVENT_OTA_DOWNLOAD_DONE);
        break;
        /* 5 consecutive failures, must stop */
    case OTA_RUN_ERROR_CONSECUTIVE_OTA_ERRORS:      
#ifdef OTA_LOOP_TESTING
        /* just for loop testing  - ignore CONSECUTIVE_OTA_ERRORS */
        OTA_DBG_PRINT("OtaRunStep: ignore it just for loop testing\r\n");
        SignalEvent(APP_EVENT_RESTART);
        break;
#endif

    case OTA_RUN_ERROR_NO_SERVER_NO_VENDOR:
    case OTA_RUN_ERROR_UNEXPECTED_STATE:
    case OTA_RUN_ERROR_SECURITY_ALERT:         /* security alert, must stop */
        OtaCount_Errors++;
        /* User could wait OTA period time and try maybe there is 
        good new version to download */
        OTA_DBG_PRINT("\r\n");
        OTA_DBG_PRINT(
            "OtaRunStep: FATAL ERROR from "
            "Ota_run %d !!!!!!!!!!!!!!!!!!!!!!!!!!!\r\n",
            Status);
        OTA_DBG_PRINT("\r\n");
        SignalEvent(APP_EVENT_OTA_ERROR);
        break;

    default:
        if(Status < 0)
        {
            OTA_DBG_PRINT(
                "OtaRunStep: Unknown negative from Ota_run %d, halt!! \r\n",
                Status);
            SignalEvent(APP_EVENT_OTA_ERROR);
        }
        else
        {
            OTA_DBG_PRINT(
                "OtaRunStep: Unknown positive status from "
                "Ota_run %d, continue \r\n",
                Status);
            SignalEvent(APP_EVENT_CONTINUE);
        }
        break;
    }

    return(0);
}

int32_t OtaIsActive()
{
    int ProcActive;
    int ProcActiveLen;

    /* check OTA process */
    OTA_get(EXTLIB_OTA_GET_OPT_IS_ACTIVE, (int32_t *)&ProcActiveLen,
            (uint8_t *)&ProcActive);

    return(ProcActive);
}

int32_t OtaImageTestingAndReset()
{
    int32_t retVal;

    OTA_DBG_PRINT("\n\n\n\n");
    OTA_DBG_PRINT("OtaImageTestingAndReset: download done\r\n");
    OTA_DBG_PRINT(
        "OtaImageTestingAndReset: call sl_Stop to move the bundle to"
        "testing state\r\n");
    sl_Stop(SL_STOP_TIMEOUT);
    OTA_DBG_PRINT(
        "OtaImageTestingAndReset: reset the platform to test the new"
        "image...\r\n");
    Platform_Reset();

    /* if we reach here, the platform does not support self reset */
    /* reset the NWP in order to test the new image */
    OTA_DBG_PRINT("\n");
    OTA_DBG_PRINT(
        "OtaImageTestingAndReset: platform does not support self reset\r\n");
    OTA_DBG_PRINT(
        "OtaImageTestingAndReset: reset the NWP to test the new image\r\n");
    OTA_DBG_PRINT("\n");
    retVal = InitSimplelink(ROLE_STA);
    /* The sl_Stop/Start will produce event APP_EVENT_STARTED */
    return(retVal);
}

static int16_t SignalEvent(e_AppEvent event)
{
    int iRetVal;
#ifdef NORTOS_SUPPORT
    struct  mq_timespec tm;
#else
    struct  timespec tm;
#endif

    /* signal provisioning task about SL Event */
    clock_gettime(CLOCK_REALTIME, &tm);
    tm.tv_sec = 0;  /* do not wait */
    tm.tv_nsec = 0;
    iRetVal = mq_timedsend(g_OtaSMQueue, (const char*)&event, 1, 0, &tm);

    if(iRetVal < 0)
    {
        UART_PRINT("unable to send to msg queue\r\n");
        LOOP_FOREVER();
    }

    return(0);
}

void* OtaTask(void *pvParameters)
{
    int iRetVal = 0;
    int8_t event;
    s_AppContext *const pCtx = &gAppCtx;
    s_TblEntry   *pEntry = NULL;
    int32_t msgqRetVal;

    /* Queue management related configurations */

    /* Create the queue */
    mq_attr attr;
    attr.mq_maxmsg = 10;         /* queue size */
    attr.mq_msgsize = sizeof(unsigned char);        /* Size of message */
    g_OtaSMQueue = mq_open("ota msg q", O_CREAT, 0, &attr);

    if(((int)g_OtaSMQueue) <= 0)
    {
        UART_PRINT("unable to create the msg queue\r\n");
        LOOP_FOREVER();
    }

    /* Configure SimpleLink in STATION role */
    iRetVal = InitSimplelink(ROLE_STA);

    if(iRetVal < 0)
    {
        UART_PRINT("Failed to initialize the device!!, halt\r\n");
        LOOP_FOREVER();
    }

    while(1)
    {
#ifdef NORTOS_SUPPORT
        /* The SimpleLink host driver architecture mandate calling 'sl_task' in
        a NO-RTOS application's main loop.       */
        /* The purpose of this call, is to handle asynchronous events and get
        flow control information sent from the NWP.*/
        /* Every event is classified and later handled by the host driver
        event handlers.                                */
        sl_Task(NULL);
#endif
        msgqRetVal = mq_receive(g_OtaSMQueue, (char*)&event, 1, NULL);

        /* if message q is empty */
        if(msgqRetVal < 0)
        {
#ifdef USE_TIRTOS
            if(pCtx->pendingEvents &= APP_EVENT_TIMEOUT)
            {
                pCtx->pendingEvents = 0;
                SignalEvent(APP_EVENT_TIMEOUT);
                continue;
            }
#endif
            SignalEvent(APP_EVENT_NULL);
            continue;
        }

        if(event != APP_EVENT_NULL)
        {
            StopAsyncEvtTimer();
        }

        /* Find Next event entry */
        pEntry = (s_TblEntry *)&gTransitionTable[pCtx->currentState][event];

        if(NULL != pEntry->p_evtHndl)
        {
            if(pEntry->p_evtHndl() < 0)
            {
                UART_PRINT("Event handler failed..!! \r\n");
                LOOP_FOREVER();
            }
        }

        /* Change state according to event */
        if(pEntry->nextState != pCtx->currentState)
        {
            pCtx->currentState = pEntry->nextState;
        }
    }
}

void SimpleLinkNetAppRequestMemFreeEventHandler(uint8_t *buffer)
{
    /* do nothing... */
}

void SimpleLinkNetAppRequestEventHandler(SlNetAppRequest_t *pNetAppRequest,
                                         SlNetAppResponse_t *pNetAppResponse)
{
    /* do nothing... */
}

#ifdef NORTOS_SUPPORT

void * mainThread(void *pvParameters)
{
    GPIO_init();

    SPI_init();
    /* Init the platform code..*/
    Platform_Init();
    Platform_TimerInit(&AsyncEvtTimerIntHandler);

    /* Init Terminal, and print App name */
    InitTerm();
    DisplayBanner();

    /* Switch off all LEDs on boards */
    GPIO_write(CONFIG_GPIO_LED_0, CONFIG_GPIO_LED_OFF);
    GPIO_write(CONFIG_GPIO_LED_1, CONFIG_GPIO_LED_OFF);

    /* Start OTA */
    OtaTask(NULL);

    return(0);
}

#else

void * mainThread(void *pvParameters)
{
    uint32_t RetVal;
    pthread_attr_t pAttrs;
    pthread_attr_t pAttrs_spawn;
    struct sched_param priParam;
    struct timespec ts = {0};

    GPIO_init();
    SPI_init();

    /* Init the platform code..*/
    Platform_Init();
    Platform_TimerInit(&AsyncEvtTimerIntHandler);

    /* Init Terminal, and print App name */
    InitTerm();
    /* Initilize the realtime clock */
    clock_settime(CLOCK_REALTIME, &ts);
    DisplayBanner();

    /* Switch off all LEDs on boards */
    GPIO_write(CONFIG_GPIO_LED_0, CONFIG_GPIO_LED_OFF);
    GPIO_write(CONFIG_GPIO_LED_1, CONFIG_GPIO_LED_OFF);

    /* create the sl_Task */
    pthread_attr_init(&pAttrs_spawn);
    priParam.sched_priority = SPAWN_TASK_PRIORITY;
    RetVal = pthread_attr_setschedparam(&pAttrs_spawn, &priParam);
    RetVal |= pthread_attr_setstacksize(&pAttrs_spawn, TASK_STACK_SIZE);

    RetVal = pthread_create(&g_spawn_thread, &pAttrs_spawn, sl_Task, NULL);

    if(RetVal)
    {
        while(1)
        {
            ;
        }
    }

    pthread_attr_init(&pAttrs);
    priParam.sched_priority = 1;
    RetVal = pthread_attr_setschedparam(&pAttrs, &priParam);
    RetVal |= pthread_attr_setstacksize(&pAttrs, TASK_STACK_SIZE);

    if(RetVal)
    {
        /* error handling */
        while(1)
        {
            ;
        }
    }

    RetVal = pthread_create(&g_ota_thread, &pAttrs, OtaTask, NULL);

    if(RetVal)
    {
        while(1)
        {
            ;
        }
    }

    return(0);
}

#endif
