/*
 * Copyright (c) 2004, 2012 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

#include <CoreFoundation/CoreFoundation.h>
#include <TargetConditionals.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/pwr_mgt/IOPMPrivate.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <mach/mach_init.h>
#include <mach/mach_port.h>
#include <mach/vm_map.h>
#include <servers/bootstrap.h>
#include <bootstrap_priv.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <notify.h>
#include "IOSystemConfiguration.h"
#include "IOPMLibPrivate.h"
#include "powermanagement.h"
#include <asl.h>

#include <unistd.h>
#include <stdlib.h>
#include <dirent.h>
#include <pthread.h>


#define pwrLogDirName "/System/Library/PowerEvents"

static const int kMaxNameLength = 128;
static mach_port_t powerd_connection = MACH_PORT_NULL;

static void _reset_connection( )
{
  powerd_connection = MACH_PORT_NULL;
}   

IOReturn _pm_connect(mach_port_t *newConnection)
{
    kern_return_t       kern_result = KERN_SUCCESS;
    
    if(!newConnection) return kIOReturnBadArgument;
    if (powerd_connection != MACH_PORT_NULL) {
        *newConnection = powerd_connection;

        return kIOReturnSuccess;
    }

    // open reference to PM configd
    kern_result = bootstrap_look_up2(bootstrap_port, 
                                     kIOPMServerBootstrapName, 
                                     &powerd_connection, 
                                     0, 
                                     BOOTSTRAP_PRIVILEGED_SERVER);    
    if(KERN_SUCCESS != kern_result) {
        *newConnection = powerd_connection =  MACH_PORT_NULL;
        asl_log(NULL, NULL, ASL_LEVEL_ERR, 
                "bootstrap_look_up2 failed with 0x%x\n", kern_result);
        return kIOReturnError;
    }
    *newConnection = powerd_connection;
    if (pthread_atfork(NULL, NULL, _reset_connection) != 0)
       powerd_connection = MACH_PORT_NULL;

    return kIOReturnSuccess;
}

IOReturn _pm_disconnect(mach_port_t connection __unused)
{
    // Do nothing. We re-use the mach port
    return kIOReturnSuccess;
}

/*****************************************************************************/
/*****************************************************************************/

/******************************************************************************
 * IOPMCopyHIDPostEventHistory
 *
 ******************************************************************************/

IOReturn IOPMCopyHIDPostEventHistory(CFArrayRef *outArray)
{
    CFDataRef               serializedData = NULL;
    vm_address_t            outBuffer = 0;
    vm_size_t               outSize = 0;
    mach_port_t             pmserverport = MACH_PORT_NULL;
    IOReturn                ret = kIOReturnError;
    int                     history_return = 0;
    
    if (kIOReturnSuccess != _pm_connect(&pmserverport))
        goto exit;

    if (KERN_SUCCESS != io_pm_hid_event_copy_history(pmserverport, 
                                &outBuffer, (mach_msg_type_number_t *) &outSize, &history_return))
    {
        goto exit;
    }

    serializedData = CFDataCreate(0, (const UInt8 *)outBuffer, outSize);
    if (serializedData) {
        *outArray = (CFArrayRef)CFPropertyListCreateWithData(0, serializedData, 0, NULL, NULL);
        CFRelease(serializedData);
    }
 
    if (*outArray)
        ret = kIOReturnSuccess;
 
    vm_deallocate(mach_task_self(), outBuffer, outSize);
    _pm_disconnect(pmserverport);
exit:
    return ret;
}

/******************************************************************************
 * IOPMGetLastWakeTime
 *
 ******************************************************************************/

IOReturn IOPMGetLastWakeTime(
    CFAbsoluteTime      *lastWakeTimeOut,
    CFTimeInterval      *adjustedForPhysicalWakeOut)
{
    IOReturn            ret;
    CFTimeInterval      lastSMCS3S0WakeInterval = 0.0;
    CFAbsoluteTime      lastWakeTime;
    struct timeval      rawLastWakeTime;
    size_t              rawLastWakeTimeSize = sizeof(rawLastWakeTime);

    if (!lastWakeTimeOut || !adjustedForPhysicalWakeOut) {
        return kIOReturnBadArgument;
    }
    
    *lastWakeTimeOut = 0.0;
    *adjustedForPhysicalWakeOut = 0.0;

    ret = sysctlbyname("kern.waketime", &rawLastWakeTime, &rawLastWakeTimeSize, NULL, 0);
    if (ret || !rawLastWakeTime.tv_sec) {
        return kIOReturnNotReady;
    }

    // Convert the timeval, which is in UNIX time, to a CFAbsoluteTime
    lastWakeTime = rawLastWakeTime.tv_sec + (rawLastWakeTime.tv_usec / 1000000.0);
    lastWakeTime -= kCFAbsoluteTimeIntervalSince1970;


    *lastWakeTimeOut = lastWakeTime;
    *adjustedForPhysicalWakeOut = lastSMCS3S0WakeInterval;

    return kIOReturnSuccess;
}


#pragma mark -
#pragma mark API

/******************************************************************************
 * IOPMCopyPowerHistory
 *
 ******************************************************************************/
IOReturn IOPMCopyPowerHistory(CFArrayRef *outArray)
{
  DIR *dp;
  struct dirent *ep;
  
  CFMutableArrayRef logs = CFArrayCreateMutable(0, 0, &kCFTypeArrayCallBacks);

  dp = opendir(pwrLogDirName);
  if(dp == NULL)
    return kIOReturnError;
  
  CFMutableDictionaryRef uuid_details;
  CFStringRef uuid;
  CFStringRef timestamp;

  char *tok;
  char *d_name;
  int fileCount = 0;

  while ((ep = readdir(dp))) {
    if(!strcmp(ep->d_name, ".") || !strcmp(ep->d_name, ".."))
      continue;
    else
     fileCount++;

    uuid_details = CFDictionaryCreateMutable(kCFAllocatorDefault,
                                             0,
                                             &kCFTypeDictionaryKeyCallBacks,
                                             &kCFTypeDictionaryValueCallBacks);
    d_name = strdup(ep->d_name);

    // Parse filename for metadata
    int part = 1;
    while((tok = strsep(&d_name, "_")) != NULL) {
      
      if(part == 1) {
        timestamp = CFStringCreateWithCString(kCFAllocatorDefault,
                                              tok,
                                              kCFStringEncodingUTF8);
        CFDictionarySetValue(uuid_details, 
                             CFSTR(kIOPMPowerHistoryTimestampKey), 
                             timestamp);
        CFRelease(timestamp);
      }
      else if(part == 2) {
        uuid = CFStringCreateWithCString(kCFAllocatorDefault, 
                                         tok, 
                                         kCFStringEncodingUTF8);
        CFDictionarySetValue(uuid_details, 
                             CFSTR(kIOPMPowerHistoryUUIDKey), 
                             uuid);
        CFRelease(uuid);
      }
      else if(part == 3) {
        // We don't want the extension .plog to be part of the
        // timestamp
        tok = strsep(&tok, ".");
        timestamp = CFStringCreateWithCString(kCFAllocatorDefault,
                                              tok,
                                              kCFStringEncodingUTF8);
        CFDictionarySetValue(uuid_details, 
                             CFSTR(kIOPMPowerHistoryTimestampCompletedKey), 
                             timestamp);
        CFRelease(timestamp);
      }
      
      part++;
    }
    
    CFArrayAppendValue(logs, uuid_details);
    CFRelease(uuid_details);
    free(d_name);
  }
  
  closedir(dp);

  if(fileCount == 0) {
    *outArray = NULL;
    return kIOReturnNotFound;
  }
  else
    *outArray = logs;

  return kIOReturnSuccess;
} 


/******************************************************************************
 * IOPMCopyPowerHistoryDetailed
 *
 ******************************************************************************/
IOReturn IOPMCopyPowerHistoryDetailed(CFStringRef UUID, CFDictionaryRef *details)//CFArrayRef *outArray)
{
    IOReturn                return_code = kIOReturnBadArgument;
    CFDataRef               serializedData = NULL;
    char                    uuid_cstr[kMaxNameLength];
    
    CFURLRef                fileURL = NULL;
    CFMutableStringRef      fileName = CFStringCreateMutable(
                                                    kCFAllocatorDefault,
                                                    255);

    CFStringAppend(fileName, CFSTR(pwrLogDirName));
    CFStringAppend(fileName, CFSTR("/"));

    if (NULL == details || NULL == UUID) {
        goto exit;
    }

    *details = NULL;

    if (!CFStringGetCString(UUID, uuid_cstr, sizeof(uuid_cstr), kCFStringEncodingMacRoman)) {
        goto exit;
    }
    
    DIR *dp;
    struct dirent *ep;

    dp = opendir(pwrLogDirName);

    if(dp == NULL) {
      return_code = kIOReturnError;
      goto exit;
    }

    while ((ep = readdir(dp))) {
      if(strstr(ep->d_name, uuid_cstr)) {
        CFStringRef uuid_file = CFStringCreateWithCString(
                                                    kCFAllocatorDefault,
                                                    ep->d_name,
                                                    kCFStringEncodingUTF8);
        CFStringAppend(fileName, uuid_file);
        fileURL = CFURLCreateWithFileSystemPath(
                                               kCFAllocatorDefault,
                                               fileName,
                                               kCFURLPOSIXPathStyle,
                                               false); 

        CFRelease(uuid_file);
      }
    }

    closedir(dp);

    if(!fileURL) {
      return_code = kIOReturnError;
      goto exit;
    }
    SInt32 errorCode;
    Boolean status = CFURLCreateDataAndPropertiesFromResource(
                                                kCFAllocatorDefault,
                                                fileURL,
                                                &serializedData,
                                                NULL,
                                                NULL,
                                                &errorCode);

    if (serializedData && status) {
        *details = (CFDictionaryRef)CFPropertyListCreateWithData(0, serializedData, 0, NULL, NULL);
        CFRelease(serializedData);
    }

    if (NULL == details) {
        return_code = kIOReturnError;
    } else {
        return_code = kIOReturnSuccess;
    }

    CFRelease(fileURL);
    CFRelease(fileName);

exit:   
    return return_code;
}

/******************************************************************************
 * IOPMSetSleepServiceCapTimeout
 *
 ******************************************************************************/
IOReturn IOPMSetSleepServicesWakeTimeCap(CFTimeInterval cap)
{
   mach_port_t             pm_server       = MACH_PORT_NULL;
   kern_return_t           return_code     = KERN_SUCCESS;
   int                     pm_mig_return = -1;

   return_code = _pm_connect(&pm_server);

   if(kIOReturnSuccess != return_code) {
      return return_code;
   }
 
   return_code = io_pm_set_sleepservice_wake_time_cap(pm_server, (int)cap, &pm_mig_return);

   _pm_disconnect(pm_server);
   if (pm_mig_return == kIOReturnSuccess)
      return return_code;
   else
      return pm_mig_return;
}

/******************************************************************************
 * IOPMSleepWakeSetUUID
 *
 ******************************************************************************/
IOReturn IOPMSleepWakeSetUUID(CFStringRef newUUID)
{
    IOReturn        ret = kIOReturnSuccess;
    io_service_t    service = IO_OBJECT_NULL;
    CFTypeRef       setObject = NULL;
    
    if (!newUUID) {
        // Clear active UUID
        setObject = kCFBooleanFalse;
    } else {
        // cache the upcoming UUID
        setObject = newUUID;
    }
    
    service = IORegistryEntryFromPath(kIOMasterPortDefault, 
            kIOPowerPlane ":/IOPowerConnection/IOPMrootDomain");

    if (IO_OBJECT_NULL != service) {
        ret = IORegistryEntrySetCFProperty(
                    service, CFSTR(kIOPMSleepWakeUUIDKey),
                    setObject);
        IOObjectRelease(service);
    }
    return ret;
}
 
/******************************************************************************
 * IOPMSleepWakeCopyUUID
 *
 ******************************************************************************/
CFStringRef IOPMSleepWakeCopyUUID(void)
{
    CFStringRef     uuidString = NULL;
    io_service_t    service = IO_OBJECT_NULL;
    
    service = IORegistryEntryFromPath(kIOMasterPortDefault, 
            kIOPowerPlane ":/IOPowerConnection/IOPMrootDomain");

    if (IO_OBJECT_NULL != service) {
        uuidString = IORegistryEntryCreateCFProperty(
                       service,
                       CFSTR(kIOPMSleepWakeUUIDKey),
                       kCFAllocatorDefault, 0);
        IOObjectRelease(service);
    }
    
    // Caller must release uuidString, if non-NULL
    return uuidString;
}


bool IOPMGetUUID(int whichUUID, char *putTheUUIDHere, int sizeOfBuffer)
{
    bool return_bool_value = false;
    
    if (kIOPMSleepWakeUUID == whichUUID)
    {
        CFStringRef bs = IOPMSleepWakeCopyUUID();
        if (bs) {
            Boolean bool_result;
            bool_result = CFStringGetCString(bs, putTheUUIDHere, sizeOfBuffer, kCFStringEncodingUTF8);
            CFRelease(bs);
            return bool_result;
        }        
        return false;
    }
    else if (kIOPMSleepServicesUUID == whichUUID) 
    {
        mach_port_t             pm_server       = MACH_PORT_NULL;
        kern_return_t           return_code     = KERN_SUCCESS;
        char                    strPtr[kPMMIGStringLength];
        int                     pm_mig_return = -1;

        return_code = _pm_connect(&pm_server);

        if(kIOReturnSuccess != return_code) {
            return false;
        }
        
        bzero(strPtr, sizeof(strPtr));
        return_code = io_pm_get_uuid(pm_server, kIOPMSleepServicesUUID, strPtr, &pm_mig_return);

        if ((KERN_SUCCESS == return_code) && (KERN_SUCCESS == pm_mig_return))
        {
            bzero(putTheUUIDHere, sizeOfBuffer);
            
            strncpy(putTheUUIDHere, strPtr, sizeOfBuffer-1);
 
            return_bool_value = true;
        }

        _pm_disconnect(pm_server);
    }

    return return_bool_value;    
}

/******************************************************************************
 * IOPMDebugTracePoint
 *
 ******************************************************************************/
IOReturn IOPMDebugTracePoint(CFStringRef facility, uint8_t *data, int dataCount)
{
    io_registry_entry_t gRoot = IO_OBJECT_NULL;
    CFNumberRef         setNum = NULL;
    IOReturn            ret = kIOReturnError;
    if (data == NULL || facility == NULL || 1 != dataCount)
        return kIOReturnBadArgument;

    gRoot = IORegistryEntryFromPath( kIOMasterPortDefault, 
        kIOPowerPlane ":/IOPowerConnection/IOPMrootDomain");

    if (IO_OBJECT_NULL == gRoot)
        return kIOReturnError;
        
    // We allow CF to treat this number as a signed integer. We're using it as a bitfield;
    // we shouldn't have to worry about sign extension when it gets unpacked.
    setNum = CFNumberCreate(0, kCFNumberSInt8Type, data);
    if (!setNum)
        goto exit;

    ret = IORegistryEntrySetCFProperty( gRoot, 
                        CFSTR(kIOPMLoginWindowSecurityDebugKey), 
                        setNum);
exit:
    if (setNum)
        CFRelease(setNum);
    if (gRoot)
        IOObjectRelease(gRoot);
    return ret;
}

/******************************************************************************
 * IOPMCopySleepWakeFailure
 *
 ******************************************************************************/
CFDictionaryRef IOPMCopySleepWakeFailure(void)
{
    CFStringRef         scFailureKey = NULL;
    CFDictionaryRef     scFailureDictionary = NULL;
    SCDynamicStoreRef   scDynStore = NULL;

    scDynStore = SCDynamicStoreCreate(0, CFSTR("IOPMSleepFailure"), NULL, NULL);
    if (!scDynStore)
        goto exit;

    scFailureKey = SCDynamicStoreKeyCreate(
                            kCFAllocatorDefault, 
                            CFSTR("%@%@/%@"),
                            kSCDynamicStoreDomainState, 
                            CFSTR("PowerManagement"), 
                            CFSTR(kIOPMDynamicStoreSleepFailureKey));
    if (!scFailureKey)
        goto exit;

    scFailureDictionary = isA_CFDictionary(SCDynamicStoreCopyValue(scDynStore, scFailureKey));

exit:
    if (scDynStore)
        CFRelease(scDynStore);    
    if (scFailureKey)
        CFRelease(scFailureKey);
    return scFailureDictionary;
}

/*****************************************************************************/
/*****************************************************************************/
/*****************************************************************************/
/*****************************************************************************/

/* __IOPMConnection is the IOKit-tracking struct to keep track of
 * open connections.
 */
typedef struct {
    uint32_t                id;
    int                     pid;
    CFStringRef             connectionName;
    
    /* for dispatch-style notifications */
    mach_port_t             mach_port;
    dispatch_source_t       dispatchDelivery;

    /* for CFRunLoop-style notifications */
    CFMachPortRef           localCFMachPort;
    CFRunLoopSourceRef      localCFMachPortRLS;

    IOPMEventHandlerType    userCallout;
    void                    *userParam;
    int                     runLoopCount;
} __IOPMConnection;


/*****************************************************************************/
/*****************************************************************************/

/* iopm_mach_port_callback
 * The caller installed this callback on a runloop of their choice.
 * Note that this code is running in the caller's runloop context; we don't have
 * it serialized.
 */
#define kMsgPayloadCount    2

typedef struct {
    mach_msg_header_t   header;
    mach_msg_body_t     body;
    uint32_t            payload[kMsgPayloadCount];
} IOPMMessageStructure;

static void iopm_mach_port_callback(
    CFMachPortRef port __unused, 
    void *msg, 
    CFIndex size __unused, 
    void *info)
{
    IOPMMessageStructure *m  = (IOPMMessageStructure *)msg;
    
    __IOPMConnection        *connection = (__IOPMConnection *)info;

    if (!connection || !connection->userCallout) {
        return;
    }

    (*(connection->userCallout))(
                    connection->userParam,
                    (IOPMConnection)connection,
                    m->payload[1],      // messageToken argument
                    m->payload[0]);     // event DATA
    
    return;
}

/*****************************************************************************/
/*****************************************************************************/

static kern_return_t _conveyMachPortToPowerd(
                                             __IOPMConnection *connection, 
                                             mach_port_t the_port,
                                             bool enable)
{
    mach_port_t             pm_server       = MACH_PORT_NULL;
    kern_return_t           return_code     = KERN_SUCCESS;
    
    return_code = _pm_connect(&pm_server);
    
    if(kIOReturnSuccess != return_code) {
        goto exit;
    }
    
    return_code = io_pm_connection_schedule_notification(pm_server, connection->id, the_port, enable ? 0:1, &return_code);
    
    _pm_disconnect(pm_server);
    
exit:
    return return_code;
}


/*****************************************************************************/
/*****************************************************************************/
#pragma mark -
#pragma mark IOPMConnection

IOReturn IOPMConnectionSetNotification(
    IOPMConnection myConnection, 
    void *param, 
    IOPMEventHandlerType handler)
{
    __IOPMConnection *connection = (__IOPMConnection *)myConnection;

    if (!connection || !handler)
        return kIOReturnBadArgument;
    
    connection->userParam = param;
    connection->userCallout = handler;

    return kIOReturnSuccess;
}

/*****************************************************************************/
/*****************************************************************************/

IOReturn IOPMConnectionScheduleWithRunLoop(
    IOPMConnection myConnection, 
    CFRunLoopRef theRunLoop,
    CFStringRef runLoopMode)
{
    __IOPMConnection *connection = (__IOPMConnection *)myConnection;

    IOReturn                return_code = kIOReturnError;
    CFMachPortContext       mpContext = { 1, (void *)connection, NULL, NULL, NULL };

    if (!connection || !theRunLoop || !runLoopMode)
        return kIOReturnBadArgument;

    if (NULL == connection->localCFMachPort)
    {
        // Create the mach port on which we'll receive mach messages
        // from PM configd.
        connection->localCFMachPort = CFMachPortCreate(
                                            kCFAllocatorDefault,
                                            iopm_mach_port_callback,
                                            &mpContext, NULL);
        
        if (connection->localCFMachPort) {
            connection->localCFMachPortRLS = CFMachPortCreateRunLoopSource(
                                            kCFAllocatorDefault,
                                            connection->localCFMachPort,
                                            0);
        }   
    }

    if (!connection->localCFMachPortRLS)
        return kIOReturnInternalError;

    // Record our new run loop.
    connection->runLoopCount++;

    CFRunLoopAddSource(theRunLoop, connection->localCFMachPortRLS, runLoopMode);

    // We have a mapping of one mach_port connected to PM configd to as many
    // CFRunLoopSources that the caller originates.
    if (1 == connection->runLoopCount)
    {
        mach_port_t notify_mach_port = MACH_PORT_NULL;

        if (connection->localCFMachPort) {
            notify_mach_port = CFMachPortGetPort(connection->localCFMachPort);
        }
    
        return_code =_conveyMachPortToPowerd(connection, notify_mach_port, true);
    }
    
    return return_code;
}

/*****************************************************************************/
/*****************************************************************************/

IOReturn IOPMConnectionUnscheduleFromRunLoop(
                                             IOPMConnection myConnection, 
                                             CFRunLoopRef theRunLoop,
                                             CFStringRef runLoopMode)
{
    __IOPMConnection    *connection = (__IOPMConnection *)myConnection;
    IOReturn            return_code = kIOReturnSuccess;
    
    if (!connection || !theRunLoop || !runLoopMode)
        return kIOReturnBadArgument;
    
    if (connection->localCFMachPort) {
        CFRunLoopRemoveSource(theRunLoop, connection->localCFMachPortRLS, runLoopMode);
    }
    
    connection->runLoopCount--;
    
    if (0 == connection->runLoopCount) 
    {
        mach_port_t notify_mach_port = MACH_PORT_NULL;

        if (connection->localCFMachPort) {
            notify_mach_port = CFMachPortGetPort(connection->localCFMachPort);
        }
    
        return_code = _conveyMachPortToPowerd(connection, notify_mach_port, false);
    }

    return return_code;
}

/*****************************************************************************/
/*****************************************************************************/

void IOPMConnectionSetDispatchQueue(
    IOPMConnection myConnection, 
    dispatch_queue_t myQueue)
{
    __IOPMConnection *connection = (__IOPMConnection *)myConnection;

    if (!connection)
        return;

    if (!myQueue) {
        /* Clean up a previously scheduled dispatch. */
        if (connection->dispatchDelivery) 
        {
            dispatch_source_cancel(connection->dispatchDelivery);
        }
        return;
    }

    if (KERN_SUCCESS != mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &connection->mach_port))
    {
        // Error allocating mach port
        return;
    }

    mach_port_insert_right(mach_task_self(), connection->mach_port, connection->mach_port, 
                           MACH_MSG_TYPE_MAKE_SEND);

    if (!(connection->dispatchDelivery
          = dispatch_source_create(DISPATCH_SOURCE_TYPE_MACH_RECV, connection->mach_port, 0, myQueue))) 
    {
        // Error creating dispatch_source
        mach_port_deallocate(mach_task_self(), connection->mach_port);
        mach_port_mod_refs(mach_task_self(), connection->mach_port, MACH_PORT_RIGHT_RECEIVE, -1);
        return;
    }
    
    dispatch_source_set_cancel_handler(connection->dispatchDelivery,
                                       ^{
                                           _conveyMachPortToPowerd(connection, connection->mach_port, false);
                                           
                                           dispatch_release(connection->dispatchDelivery);
                                           connection->dispatchDelivery = 0;
                                           
                                           mach_port_mod_refs(mach_task_self(), connection->mach_port, MACH_PORT_RIGHT_RECEIVE, -1);
                                           mach_port_deallocate(mach_task_self(), connection->mach_port);
                                           connection->mach_port = MACH_PORT_NULL;
                                       });
    
    dispatch_source_set_event_handler(connection->dispatchDelivery,      
                                      ^{
                                        struct {
                                            IOPMMessageStructure    m;
                                            mach_msg_trailer_t      trailer;
                                        } msg;

                                        bzero(&msg, sizeof(msg));
                                        kern_return_t status = mach_msg( ( void * )&msg,
                                                         MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                                                         0,
                                                         sizeof( msg ),
                                                         connection->mach_port,
                                                         0,
                                                         MACH_PORT_NULL );
                                                                                                                                        
                                        if (!connection || !connection->userCallout
                                            || (KERN_SUCCESS != status)) 
                                        {
                                            return;
                                        }
                                        
                                        (*(connection->userCallout))(connection->userParam,
                                                                 (IOPMConnection)connection,
                                                                 msg.m.payload[1], msg.m.payload[0]);
                                    });
    
    dispatch_resume(connection->dispatchDelivery);
    
    _conveyMachPortToPowerd(connection, connection->mach_port, true);
    
    return;
}

/*****************************************************************************/
/*****************************************************************************/

IOReturn IOPMConnectionCreate(
    CFStringRef myName,
    IOPMCapabilityBits interests,
    IOPMConnection *newConnection)
{
    __IOPMConnection        *connection = NULL;

    mach_port_t             pm_server = MACH_PORT_NULL;
    int                     return_code = kIOReturnError;
    IOReturn                err = kIOReturnError;
    kern_return_t           kern_result = KERN_SUCCESS;

    char                    arg_name_str[kMaxNameLength];
    uint32_t                new_connection_id = 0;

    // * vet argument newConnection
    // * and create new connection
    if (!newConnection)
        return kIOReturnBadArgument;
        
    *newConnection = NULL;

    err = _pm_connect(&pm_server);
    if(kIOReturnSuccess != err) {
        return_code = kIOReturnInternalError;
        goto exit;
    }

    // * vet argument 'interests'
    // A caller specifying 0 interests would get no notifications
    if (0 == interests) {
        return_code = kIOReturnBadArgument;
        goto exit;
    }

    // * vet argument 'myName'
    if (!myName || (kMaxNameLength < CFStringGetLength(myName))) {
        return_code = kIOReturnBadArgument;
        goto exit;
    }
    CFStringGetCString( myName, arg_name_str,
                        sizeof(arg_name_str), kCFStringEncodingMacRoman);


    connection = calloc(1, sizeof(__IOPMConnection));
    if (!connection) {
        return_code = kIOReturnInternalError;
        goto exit;
    }

    kern_result = io_pm_connection_create(
                                    pm_server,
                                    mach_task_self(),
                                    arg_name_str,
                                    interests,
                                    &new_connection_id,
                                    &return_code);

    if (KERN_SUCCESS != kern_result) {
        return_code = kern_result;
        goto exit;
    }
    
    connection->id = (int)new_connection_id;
    *newConnection = (void *)connection;    
    
    return_code = kIOReturnSuccess;

exit:
    if (MACH_PORT_NULL != pm_server) {
        _pm_disconnect(pm_server);
    }

    return return_code;

}

/*****************************************************************************/
/*****************************************************************************/

IOReturn IOPMConnectionRelease(IOPMConnection connection)
{
    __IOPMConnection    *connection_private = (__IOPMConnection *)connection;
    IOReturn            return_code = kIOReturnError;
    mach_port_t         pm_server = MACH_PORT_NULL;
    kern_return_t       kern_result;
    IOReturn            err;
    
    err = _pm_connect(&pm_server);
    if(kIOReturnSuccess != err) {
        return_code = kIOReturnInternalError;
        goto exit;
    }
    
#if !TARGET_OS_EMBEDDED
    if (connection_private->dispatchDelivery) {
        IOPMConnectionSetDispatchQueue(connection, NULL);
    }
#endif 

    kern_result = io_pm_connection_release(pm_server, 
                                            connection_private->id, 
                                            &return_code);
    if (kern_result != KERN_SUCCESS) {
        return_code = kern_result;
    }
exit:
    if (MACH_PORT_NULL != pm_server) {
        _pm_disconnect(pm_server);
    }

    return return_code;
}

/*****************************************************************************/
/*****************************************************************************/

IOReturn IOPMConnectionAcknowledgeEvent(
    IOPMConnection connect, 
    IOPMConnectionMessageToken token)
{
#if TARGET_OS_EMBEDDED
    (void)connect;
    (void)token;
    
    return kIOReturnUnsupported;
#else
    return IOPMConnectionAcknowledgeEventWithOptions(
                           connect, token, NULL);
#endif /* TARGET_OS_EMBEDDED */
}


/*****************************************************************************/
/*****************************************************************************/

IOReturn IOPMConnectionAcknowledgeEventWithOptions(
    IOPMConnection myConnection, 
    IOPMConnectionMessageToken token, 
    CFDictionaryRef options)
{
#if TARGET_OS_EMBEDDED
    (void)myConnection;
    (void)token;
    (void)options;
    
    return kIOReturnUnsupported;
#else
    __IOPMConnection    *connection = (__IOPMConnection *)myConnection;

    IOReturn            return_code = kIOReturnError;
    mach_port_t         pm_server = MACH_PORT_NULL;
    kern_return_t       kern_result;
    IOReturn            err;

    CFDataRef           serializedData = NULL;    
    vm_offset_t         buffer_ptr = 0;
    size_t              buffer_size = 0;
    
    err = _pm_connect(&pm_server);
    if(kIOReturnSuccess != err) {
        return_code = kIOReturnInternalError;
        goto exit;
    }
    
    if (options) 
    {
        serializedData = CFPropertyListCreateData(
                                    kCFAllocatorDefault,
                                    (CFPropertyListRef)options,
                                    kCFPropertyListBinaryFormat_v1_0, 0, NULL);
            
        if (serializedData)
        {
            buffer_ptr = (vm_offset_t)CFDataGetBytePtr(serializedData);
            buffer_size = (size_t)CFDataGetLength(serializedData);
        
        }
    }
    
    kern_result = io_pm_connection_acknowledge_event(pm_server, 
                                    (uint32_t)connection->id, 
                                    (uint32_t)token,
                                    buffer_ptr, 
                                    buffer_size,
                                    &return_code);

    if (kern_result != KERN_SUCCESS) {
        return_code = kern_result;
    }
exit:
    if (MACH_PORT_NULL != pm_server) {
        _pm_disconnect(pm_server);
    }

    if (serializedData) CFRelease(serializedData);

    return return_code;
#endif /* TARGET_OS_EMBEDDED */
}

/*****************************************************************************/
/*****************************************************************************/

IOReturn IOPMCopyConnectionStatus(int statusSelector, CFTypeRef *output)
{
    int                 return_code = kIOReturnError;
    mach_port_t         pm_server = MACH_PORT_NULL;
    kern_return_t       kern_result;
    IOReturn            err;
    
    vm_offset_t         buffer_ptr = 0;
    mach_msg_type_number_t    buffer_size = 0;
 
    CFDictionaryRef     *dictionaryOutput = (CFDictionaryRef *)output;
 
    err = _pm_connect(&pm_server);
    if(kIOReturnSuccess != err) {
        return_code = kIOReturnInternalError;
        goto exit;
    }
    
    // TODO: serialize passed-in options into buffer_ptr & buffer_size
    *dictionaryOutput = NULL;
    
    
    kern_result = io_pm_connection_copy_status(pm_server, 
                                            (uint32_t)statusSelector, 
                                            &buffer_ptr, 
                                            &buffer_size,
                                            &return_code);

    if (kern_result != KERN_SUCCESS) {
        return_code = kern_result;
    }
exit:
    if (MACH_PORT_NULL != pm_server) {
        _pm_disconnect(pm_server);
    }

    return return_code;
}

/*****************************************************************************/
/*****************************************************************************/

#define SYSTEM_ON_CAPABILITIES (kIOPMCapabilityCPU | kIOPMCapabilityVideo | kIOPMCapabilityAudio \
                                | kIOPMCapabilityNetwork | kIOPMCapabilityDisk)

IOPMCapabilityBits IOPMConnectionGetSystemCapabilities(void)
{

    mach_port_t             pm_server = MACH_PORT_NULL;
    kern_return_t           kern_result;
    IOPMCapabilityBits      ret_cap = SYSTEM_ON_CAPABILITIES;
    IOReturn                return_code = kIOReturnError;

    return_code = _pm_connect(&pm_server);

    if(pm_server == MACH_PORT_NULL)
      return ret_cap; 

    kern_result = io_pm_get_capability_bits(pm_server, &ret_cap, &return_code);

    _pm_disconnect(pm_server);

    return ret_cap;


}


bool IOPMIsADarkWake(IOPMCapabilityBits c)
{
    return ((c & kIOPMCapabilityCPU) && !(c & kIOPMCapabilityVideo));
}

bool IOPMAllowsBackgroundTask(IOPMCapabilityBits c)
{
    return (0 != (c & kIOPMCapabilityBackgroundTask));
}

bool IOPMAllowsPushServiceTask(IOPMCapabilityBits c)
{
    return (0 != (c & kIOPMCapabilityPushServiceTask));
}

bool IOPMIsAUserWake(IOPMCapabilityBits c)
{
    return (0 != (c & kIOPMCapabilityVideo));
}

bool IOPMIsASleep(IOPMCapabilityBits c)
{
    return (0 == (c & kIOPMCapabilityCPU));
}

bool IOPMGetCapabilitiesDescription(char *buf, int buf_size, IOPMCapabilityBits in_caps)
{
    uint64_t caps = (uint64_t)in_caps;
    int printed_total = 0;
    char *on_sleep_dark = "";
    
    if (IOPMIsASleep(caps))
    {
        on_sleep_dark = "Sleep";
    } else if (IOPMIsADarkWake(caps))
    {
        on_sleep_dark = "DarkWake";
    } else if (IOPMIsAUserWake(caps))
    {
        on_sleep_dark = "FullWake";
    } else
    {
        on_sleep_dark = "Unknown";
    }
    
    printed_total = snprintf(buf, buf_size, "%s:%s%s%s%s%s%s%s",
                             on_sleep_dark,
                             (caps & kIOPMCapabilityCPU) ? "cpu ":"<off> ",
                             (caps & kIOPMCapabilityDisk) ? "disk ":"",
                             (caps & kIOPMCapabilityNetwork) ? "net ":"",
                             (caps & kIOPMCapabilityAudio) ? "aud ":"",
                             (caps & kIOPMCapabilityVideo) ? "vid ":"",
                             (caps & kIOPMCapabilityPushServiceTask) ? "push ":"",
                             (caps & kIOPMCapabilityBackgroundTask) ? "bg ":"");
    
    return (printed_total <= buf_size);
}


/*****************************************************************************/
/*****************************************************************************/
#pragma mark -
#pragma mark SleepServices

bool IOPMGetSleepServicesActive(void)
{
    int         token = 0;
    uint64_t    payload = 0;

    if (NOTIFY_STATUS_OK == notify_register_check(kIOPMSleepServiceActiveNotifyName, &token)) 
    {
        notify_get_state(token, &payload);
        notify_cancel(token);
    }    
    
    return ((payload &  kIOPMSleepServiceActiveNotifyBit) ? true : false);
}

/*****************************************************************************/
/*****************************************************************************/
#pragma mark -
#pragma mark Power History

IOReturn IOPMSetPowerHistoryBookmark(char* uuid) {
	
	IOReturn                err;
	mach_port_t             pm_server = MACH_PORT_NULL;
	kern_return_t           kern_result;
	
	err = _pm_connect(&pm_server);
	
  if(pm_server == MACH_PORT_NULL)
	  return kIOReturnNotReady; 
	
  kern_result = io_pm_set_power_history_bookmark(pm_server, uuid);
	
  _pm_disconnect(pm_server);
	
	return kern_result;
}

#if TARGET_OS_EMBEDDED
IOReturn IOPMSetDebugFlags(uint32_t newFlags __unused, 
            uint32_t *oldFlags __unused)
{
#else
IOReturn IOPMSetDebugFlags(uint32_t newFlags, uint32_t *oldFlags)
{

    mach_port_t             pm_server = MACH_PORT_NULL;
    kern_return_t           kern_result;
    uint32_t                flags;
    IOReturn                return_code = kIOReturnError;

    return_code = _pm_connect(&pm_server);

    if(pm_server == MACH_PORT_NULL)
      return kIOReturnNotReady; 

    kern_result = io_pm_set_debug_flags(pm_server, newFlags, &flags, &return_code);

    _pm_disconnect(pm_server);

    if ( kern_result == KERN_SUCCESS && return_code == kIOReturnSuccess) {
        if (oldFlags) 
            *oldFlags = flags;
        return kIOReturnSuccess;
    }
    else 
        return return_code;

#endif
    return kIOReturnSuccess;
	
}

#if TARGET_OS_EMBEDDED
IOReturn IOPMSetBTWakeInterval(uint32_t newInterval __unused, 
      uint32_t *oldInterval __unused)
{
#else
IOReturn IOPMSetBTWakeInterval(uint32_t newInterval, uint32_t *oldInterval)
{

    mach_port_t             pm_server = MACH_PORT_NULL;
    kern_return_t           kern_result;
    uint32_t                interval = 0;
    IOReturn                return_code = kIOReturnError;

    return_code = _pm_connect(&pm_server);

    if(pm_server == MACH_PORT_NULL)
      return kIOReturnNotReady; 

    kern_result = io_pm_set_bt_wake_interval(pm_server, newInterval, &interval, &return_code);

    _pm_disconnect(pm_server);

    if ( kern_result == KERN_SUCCESS && return_code == kIOReturnSuccess) {
        if (oldInterval) 
            *oldInterval = interval;
        return kIOReturnSuccess;
    }
    else 
        return return_code;

#endif
    return kIOReturnSuccess;
	
}
/*****************************************************************************/
/*****************************************************************************/

/*****************************************************************************/
/*****************************************************************************/

#if 0
bool IOPMSystemPowerStateSupportsAcknowledgementOption(
    IOPMCapabilityBits stateDescriptor,
    CFStringRef acknowledgementOption)
{
    if (!acknowledgementOption)
        return false;

    if ( 0 != (stateDescriptor & kIOPMSytemPowerStateCapabilitiesMask) )
    {
        // The flags date & requiredcapabilities are only valid on going to sleep transitions
        return false;    
    }

    if (CFEqual(acknowledgementOption, kIOPMAcknowledgmentOptionSystemCapabilityRequirements)
        || CFEqual(acknowledgementOption, kIOPMAcknowledgmentOptionWakeDate))
    {
        return true;
    }

    return false;
}
#endif
