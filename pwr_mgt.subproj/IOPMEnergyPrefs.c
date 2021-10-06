/*
 * Copyright (c) 2001 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * The contents of this file constitute Original Code as defined in and
 * are subject to the Apple Public Source License Version 1.1 (the
 * "License").  You may not use this file except in compliance with the
 * License.  Please obtain a copy of the License at
 * http://www.apple.com/publicsource and read it before using this file.
 * 
 * This Original Code and all software distributed under the License are
 * distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE OR NON-INFRINGEMENT.  Please see the
 * License for the specific language governing rights and limitations
 * under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

#include <SystemConfiguration/SystemConfiguration.h>
#include <SystemConfiguration/SCValidation.h>
#include <SystemConfiguration/SCPreferencesPrivate.h>
#include <SystemConfiguration/SCDynamicStorePrivate.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/ps/IOPowerSourcesPrivate.h>
#include <IOKit/IOCFSerialize.h>
#include <IOKit/IOHibernatePrivate.h>
#include <servers/bootstrap.h>
#include <sys/syslog.h>
#include "IOPMLib.h"
#include "IOPMLibPrivate.h"
#include "powermanagement.h"

#include <IOKit/IOKitLib.h>
#include <IOKit/IOBSD.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/mount.h>


#define kIOPMPrefsPath              CFSTR("com.apple.PowerManagement.xml")
#define kIOPMAppName                CFSTR("PowerManagement configd")

// 2936060
#ifndef kIODisplayDimAggressiveness
#define kIODisplayDimAggressiveness iokit_family_err(sub_iokit_graphics, 3)
#endif 

#define kIOPMNumPMProfiles              5

/* Default Energy Saver settings for IOPMCopyPMPreferences
 * 
 *      AC
 */
#define kACMinutesToDim                 5
#define kACMinutesToSpin                10
#define kACMinutesToSleep               10
#define kACWakeOnRing                   0
#define kACAutomaticRestart             0
#define kACWakeOnLAN                    1
#define kACReduceProcessorSpeed         0
#define kACDynamicPowerStep             1
#define kACSleepOnPowerButton           1
#define kACWakeOnClamshell              1
#define kACWakeOnACChange               0
#define kACReduceBrightness             0
#define kACDisplaySleepUsesDim          1
#define kACMobileMotionModule           1

/*
 *      Battery
 */
#define kBatteryMinutesToDim            5
#define kBatteryMinutesToSpin           5
#define kBatteryMinutesToSleep          5
#define kBatteryWakeOnRing              0
#define kBatteryAutomaticRestart        0
#define kBatteryWakeOnLAN               0
#define kBatteryReduceProcessorSpeed    0
#define kBatteryDynamicPowerStep        1
#define kBatterySleepOnPowerButton      0
#define kBatteryWakeOnClamshell         1
#define kBatteryWakeOnACChange          0
#define kBatteryReduceBrightness        1
#define kBatteryDisplaySleepUsesDim     1
#define kBatteryMobileMotionModule      1

/*
 *      UPS
 */
#define kUPSMinutesToDim             kACMinutesToDim
#define kUPSMinutesToSpin                kACMinutesToSpin
#define kUPSMinutesToSleep               kACMinutesToSleep
#define kUPSWakeOnRing                   kACWakeOnRing
#define kUPSAutomaticRestart             kACAutomaticRestart
#define kUPSWakeOnLAN                    kACWakeOnLAN
#define kUPSReduceProcessorSpeed         kACReduceProcessorSpeed
#define kUPSDynamicPowerStep             kACDynamicPowerStep
#define kUPSSleepOnPowerButton           kACSleepOnPowerButton
#define kUPSWakeOnClamshell              kACWakeOnClamshell
#define kUPSWakeOnACChange               kACWakeOnACChange
#define kUPSReduceBrightness             kACReduceBrightness
#define kUPSDisplaySleepUsesDim          kACDisplaySleepUsesDim
#define kUPSMobileMotionModule           kACMobileMotionModule

#define kIOHibernateDefaultFile     "/var/vm/sleepimage"
enum { kIOHibernateMinFreeSpace     = 750*1024ULL*1024ULL }; /* 750Mb */

#define kIOPMNumPMFeatures      15

static char *energy_features_array[kIOPMNumPMFeatures] = {
    kIOPMDisplaySleepKey, 
    kIOPMDiskSleepKey,
    kIOPMSystemSleepKey,
    kIOPMWakeOnRingKey,
    kIOPMRestartOnPowerLossKey,
    kIOPMWakeOnLANKey,
    kIOPMReduceSpeedKey,
    kIOPMDynamicPowerStepKey,
    kIOPMSleepOnPowerButtonKey,
    kIOPMWakeOnClamshellKey,
    kIOPMWakeOnACChangeKey,
    kIOPMReduceBrightnessKey,
    kIOPMDisplaySleepUsesDimKey,
    kIOPMMobileMotionModuleKey,
    kIOHibernateModeKey
};

static const unsigned int battery_defaults_array[] = {
    kBatteryMinutesToDim,
    kBatteryMinutesToSpin,
    kBatteryMinutesToSleep,
    kBatteryWakeOnRing,
    kBatteryAutomaticRestart,
    kBatteryWakeOnLAN,
    kBatteryReduceProcessorSpeed,
    kBatteryDynamicPowerStep,
    kBatterySleepOnPowerButton,
    kBatteryWakeOnClamshell,
    kBatteryWakeOnACChange,
    kBatteryReduceBrightness,
    kBatteryDisplaySleepUsesDim,
    kBatteryMobileMotionModule,
    kIOHibernateModeOn | kIOHibernateModeSleep  /* safe sleep mode */
};

static const unsigned int ac_defaults_array[] = {
    kACMinutesToDim,
    kACMinutesToSpin,
    kACMinutesToSleep,
    kACWakeOnRing,
    kACAutomaticRestart,
    kACWakeOnLAN,
    kACReduceProcessorSpeed,
    kACDynamicPowerStep,
    kACSleepOnPowerButton,
    kACWakeOnClamshell,
    kACWakeOnACChange,
    kACReduceBrightness,
    kACDisplaySleepUsesDim,
    kACMobileMotionModule,
    kIOHibernateModeOn | kIOHibernateModeSleep  /* safe sleep mode */
};

static const unsigned int ups_defaults_array[] = {
    kUPSMinutesToDim,
    kUPSMinutesToSpin,
    kUPSMinutesToSleep,
    kUPSWakeOnRing,
    kUPSAutomaticRestart,
    kUPSWakeOnLAN,
    kUPSReduceProcessorSpeed,
    kUPSDynamicPowerStep,
    kUPSSleepOnPowerButton,
    kUPSWakeOnClamshell,
    kUPSWakeOnACChange,
    kUPSReduceBrightness,
    kUPSDisplaySleepUsesDim,
    kUPSMobileMotionModule,
    kIOHibernateModeOn | kIOHibernateModeSleep  /* safe sleep mode */
};

/* IOPMRootDomain property keys for default settings
 */
#define kIOPMSystemDefaultProfilesKey "SystemPowerProfiles"
#define kIOPMSystemDefaultOverrideKey "SystemPowerProfileOverrideDict"

/* Keys for Cheetah Energy Settings shim
 */
#define kCheetahDimKey                          CFSTR("MinutesUntilDisplaySleeps")
#define kCheetahDiskKey                         CFSTR("MinutesUntilHardDiskSleeps")
#define kCheetahSleepKey                        CFSTR("MinutesUntilSystemSleeps")
#define kCheetahRestartOnPowerLossKey           CFSTR("RestartOnPowerLoss")
#define kCheetahWakeForNetworkAccessKey         CFSTR("WakeForNetworkAdministrativeAccess")
#define kCheetahWakeOnRingKey                   CFSTR("WakeOnRing")

// Supported Feature bitfields for IOPMrootDomain Supported Features
enum {
    kIOPMSupportedOnAC      = 1<<0,
    kIOPMSupportedOnBatt    = 1<<1,
    kIOPMSupportedOnUPS     = 1<<2
};

// Forwards
static CFArrayRef       _createDefaultSystemProfiles();


/* IOPMAggressivenessFactors
 *
 * The form of data that the kernel understands.
 */
typedef struct {
    unsigned int        fMinutesToDim;
    unsigned int        fMinutesToSpin;
    unsigned int        fMinutesToSleep;
    unsigned int        fWakeOnLAN;
    unsigned int        fWakeOnRing;
    unsigned int        fAutomaticRestart;
    unsigned int        fSleepOnPowerButton;
    unsigned int        fWakeOnClamshell;
    unsigned int        fWakeOnACChange;
    unsigned int        fDisplaySleepUsesDimming;
    unsigned int        fMobileMotionModule;
} IOPMAggressivenessFactors;

Boolean _IOReadBytesFromFile(CFAllocatorRef alloc, const char *path, void **bytes,
                CFIndex *length, CFIndex maxLength);

static int getDefaultEnergySettings(CFMutableDictionaryRef sys)
{
    CFMutableDictionaryRef  batt = NULL;
    CFMutableDictionaryRef  ac = NULL;
    CFMutableDictionaryRef  ups = NULL;
    int             i;
    CFNumberRef     val;
    CFStringRef     key;


    batt=(CFMutableDictionaryRef)CFDictionaryGetValue(sys, CFSTR(kIOPMBatteryPowerKey));
    ac=(CFMutableDictionaryRef)CFDictionaryGetValue(sys, CFSTR(kIOPMACPowerKey));
    ups=(CFMutableDictionaryRef)CFDictionaryGetValue(sys, CFSTR(kIOPMUPSPowerKey));

    /*
     * Note that in the following "poplulation" loops, we're using CFDictionaryAddValue rather
     * than CFDictionarySetValue. If a value is already present AddValue will not replace it.
     */
    
    /* 
     * Populate default battery dictionary 
     */
    
    if(batt) {
        for(i=0; i<kIOPMNumPMFeatures; i++)
        {
            key = CFStringCreateWithCString(kCFAllocatorDefault, energy_features_array[i], kCFStringEncodingMacRoman);
            val = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &battery_defaults_array[i]);
            CFDictionaryAddValue(batt, key, val);
            CFRelease(key);
            CFRelease(val);
        }
        CFDictionaryAddValue(batt, CFSTR(kIOHibernateFileKey), CFSTR(kIOHibernateDefaultFile));
        CFDictionarySetValue(sys, CFSTR(kIOPMBatteryPowerKey), batt);
    }

    /* 
     * Populate default AC dictionary 
     */
    if(ac)
    {
        for(i=0; i<kIOPMNumPMFeatures; i++)
        {
            key = CFStringCreateWithCString(kCFAllocatorDefault, energy_features_array[i], kCFStringEncodingMacRoman);
            val = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &ac_defaults_array[i]);
            CFDictionaryAddValue(ac, key, val);
            CFRelease(key);
            CFRelease(val);
        }
        CFDictionaryAddValue(ac, CFSTR(kIOHibernateFileKey), CFSTR(kIOHibernateDefaultFile));
        CFDictionarySetValue(sys, CFSTR(kIOPMACPowerKey), ac);
    }
    
    /* 
     * Populate default UPS dictionary 
     */
    if(ups) {
        for(i=0; i<kIOPMNumPMFeatures; i++)
        {
            key = CFStringCreateWithCString(kCFAllocatorDefault, energy_features_array[i], kCFStringEncodingMacRoman);
            val = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &ups_defaults_array[i]);
            CFDictionaryAddValue(ups, key, val);
            CFRelease(key);
            CFRelease(val);
        }
        CFDictionaryAddValue(ups, CFSTR(kIOHibernateFileKey), CFSTR(kIOHibernateDefaultFile));
        CFDictionarySetValue(sys, CFSTR(kIOPMUPSPowerKey), ups);
    }

    return 0;
}

static io_registry_entry_t  getPMRootDomainRef(void)
{
    return IOServiceGetMatchingService( kIOMasterPortDefault, 
                                IOServiceNameMatching("IOPMrootDomain"));
}

static int 
ProcessHibernateSettings(CFDictionaryRef dict, io_registry_entry_t rootDomain)
{
    IOReturn	ret;
    CFTypeRef	obj;
    CFNumberRef modeNum;
    SInt32      modeValue = 0;
    CFURLRef	url = NULL;
    Boolean	createFile = false;
    Boolean	haveFile = false;
    struct stat statBuf;
    char	path[MAXPATHLEN];
    int		fd;
    long long	size;
    size_t	len;
    fstore_t	prealloc;
    uint64_t	filesize;

    if ((modeNum = CFDictionaryGetValue(dict, CFSTR(kIOHibernateModeKey)))
      && isA_CFNumber(modeNum))
	CFNumberGetValue(modeNum, kCFNumberSInt32Type, &modeValue);
    else
	modeNum = NULL;

    if (modeValue
      && (obj = CFDictionaryGetValue(dict, CFSTR(kIOHibernateFileKey)))
      && isA_CFString(obj))
    do
    {
	url = CFURLCreateWithFileSystemPath(kCFAllocatorDefault,
	    obj, kCFURLPOSIXPathStyle, true);

	if (!url || !CFURLGetFileSystemRepresentation(url, TRUE, path, MAXPATHLEN))
	    break;

	len = sizeof(size);
	if (sysctlbyname("hw.memsize", &size, &len, NULL, 0))
	    break;
	filesize = size;

	if (0 == stat(path, &statBuf))
	{
	    if ((S_IFBLK == (S_IFMT & statBuf.st_mode)) 
                || (S_IFCHR == (S_IFMT & statBuf.st_mode)))
	    {
                haveFile = true;
	    }
	    else if (S_IFREG == (S_IFMT & statBuf.st_mode))
            {
                if (statBuf.st_size >= filesize)
                    haveFile = true;
                else
                    createFile = true;
            }
	    else
		break;
	}
	else
	    createFile = true;

	if (createFile)
	{
            do
            {
                char *    patchpath, save = 0;
		struct    statfs sfs;
                u_int64_t fsfree;

                fd = -1;

		/*
		 * get rid of the filename at the end of the file specification
		 * we only want the portion of the pathname that should already exist
		 */
		if ((patchpath = strrchr(path, '/')))
                {
                    save = *patchpath;
                    *patchpath = 0;
                }

	        if (-1 == statfs(path, &sfs))
                    break;

                fsfree = ((u_int64_t)sfs.f_bfree * (u_int64_t)sfs.f_bsize);
                if ((fsfree - filesize) < kIOHibernateMinFreeSpace)
                    break;

                if (patchpath)
                    *patchpath = save;
                fd = open(path, O_CREAT | O_TRUNC | O_RDWR);
                if (-1 == fd)
                    break;
                if (-1 == fchmod(fd, 01600))
                    break;
        
                prealloc.fst_flags = F_ALLOCATEALL; // F_ALLOCATECONTIG
                prealloc.fst_posmode = F_PEOFPOSMODE;
                prealloc.fst_offset = 0;
                prealloc.fst_length = filesize;
                if (((-1 == fcntl(fd, F_PREALLOCATE, (int) &prealloc))
                    || (-1 == fcntl(fd, F_SETSIZE, &prealloc.fst_length)))
                && (-1 == ftruncate(fd, prealloc.fst_length)))
                    break;

                haveFile = true;
            }
            while (false);
            if (-1 != fd)
            {
                close(fd);
                if (!haveFile)
                    unlink(path);
            }
	}

        if (!haveFile)
            break;

#ifdef __i386__
#define kBootXPath		"/System/Library/CoreServices/boot.efi"
#define kBootXSignaturePath	"/System/Library/Caches/com.apple.bootefisignature"
#else
#define kBootXPath		"/System/Library/CoreServices/BootX"
#define kBootXSignaturePath	"/System/Library/Caches/com.apple.bootxsignature"
#endif
#define kCachesPath		"/System/Library/Caches"
#define	kGenSignatureCommand	"/bin/cat " kBootXPath " | /usr/bin/openssl dgst -sha1 -hex -out " kBootXSignaturePath


        struct stat bootx_stat_buf;
        struct stat bootsignature_stat_buf;
    
        if (0 != stat(kBootXPath, &bootx_stat_buf))
            break;

        if ((0 != stat(kBootXSignaturePath, &bootsignature_stat_buf))
         || (bootsignature_stat_buf.st_mtime != bootx_stat_buf.st_mtime))
        {
	    if (-1 == stat(kCachesPath, &bootsignature_stat_buf))
	    {
		mkdir(kCachesPath, 0777);
		chmod(kCachesPath, 0777);
	    }

            // generate signature file
            if (0 != system(kGenSignatureCommand))
               break;

            // set mod time to that of source
            struct timeval fileTimes[2];
	    TIMESPEC_TO_TIMEVAL(&fileTimes[0], &bootx_stat_buf.st_atimespec);
	    TIMESPEC_TO_TIMEVAL(&fileTimes[1], &bootx_stat_buf.st_mtimespec);
            if ((0 != utimes(kBootXSignaturePath, fileTimes)))
                break;
        }


        // send signature to kernel
	CFAllocatorRef alloc;
        void *         sigBytes;
        CFIndex        sigLen;

	alloc = CFRetain(CFAllocatorGetDefault());
        if (_IOReadBytesFromFile(alloc, kBootXSignaturePath, &sigBytes, &sigLen, 0))
            ret = sysctlbyname("kern.bootsignature", NULL, NULL, sigBytes, sigLen);
        else
            ret = -1;
        CFAllocatorDeallocate(alloc, sigBytes);
	CFRelease(alloc);
        if (0 != ret)
            break;

        ret = IORegistryEntrySetCFProperty(rootDomain, CFSTR(kIOHibernateFileKey), obj);
    }
    while (false);

    if (modeNum)
	ret = IORegistryEntrySetCFProperty(rootDomain, CFSTR(kIOHibernateModeKey), modeNum);

    if ((obj = CFDictionaryGetValue(dict, CFSTR(kIOHibernateFreeRatioKey)))
      && isA_CFNumber(obj))
    {
	ret = IORegistryEntrySetCFProperty(rootDomain, CFSTR(kIOHibernateFreeRatioKey), obj);
    }
    if ((obj = CFDictionaryGetValue(dict, CFSTR(kIOHibernateFreeTimeKey)))
      && isA_CFNumber(obj))
    {
	ret = IORegistryEntrySetCFProperty(rootDomain, CFSTR(kIOHibernateFreeTimeKey), obj);
    }

    if (url) CFRelease(url);
    return 0;
}

static int sendEnergySettingsToKernel(
    CFDictionaryRef                 System, 
    CFStringRef                     prof, 
    IOPMAggressivenessFactors       *p)
{
    io_registry_entry_t             PMRootDomain = MACH_PORT_NULL;
    io_connect_t        		    PM_connection = MACH_PORT_NULL;
    CFTypeRef                       power_source_info = NULL;
    CFStringRef                     providing_power = NULL;
    IOReturn    		            err;
    IOReturn                        ret;
    CFNumberRef                     number1;
    CFNumberRef                     number0;
    int                             type;
    UInt32                          i;
    
    i = 1;
    number1 = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &i);
    i = 0;
    number0 = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &i);
    if(!number0 || !number1) return -1;
    
    PMRootDomain = getPMRootDomainRef();
    if(!PMRootDomain) return -1;

    PM_connection = IOPMFindPowerManagement(0);
    if ( !PM_connection ) return -1;

    // Determine type of power source
    power_source_info = IOPSCopyPowerSourcesInfo();
    if(power_source_info) {
        providing_power = IOPSGetProvidingPowerSourceType(power_source_info);
    }
    
    type = kPMMinutesToDim;
    err = IOPMSetAggressiveness(PM_connection, type, p->fMinutesToDim);

    type = kPMMinutesToSpinDown;
    err = IOPMSetAggressiveness(PM_connection, type, p->fMinutesToSpin);

    type = kPMMinutesToSleep;
    err = IOPMSetAggressiveness(PM_connection, type, p->fMinutesToSleep);

    // Wake on LAN
    if(true == IOPMFeatureIsAvailable(CFSTR(kIOPMWakeOnLANKey), providing_power))
    {
        type = kPMEthernetWakeOnLANSettings;
        err = IOPMSetAggressiveness(PM_connection, type, p->fWakeOnLAN);
    } else {
        // Even if WakeOnLAN is reported as not supported, broadcast 0 as 
        // value. We may be on a supported machine, just on battery power.
        // Wake on LAN is not supported on battery power on PPC hardware.
        type = kPMEthernetWakeOnLANSettings;
        err = IOPMSetAggressiveness(PM_connection, type, 0);
    }
    
    // Display Sleep Uses Dim
    if(true == IOPMFeatureIsAvailable(CFSTR(kIOPMDisplaySleepUsesDimKey), providing_power))
    {
        type = kIODisplayDimAggressiveness;
        err = IOPMSetAggressiveness(PM_connection, type, p->fDisplaySleepUsesDimming);
    }    
    
    // Wake On Ring
    if(true == IOPMFeatureIsAvailable(CFSTR(kIOPMWakeOnRingKey), providing_power))
    {
        ret = IORegistryEntrySetCFProperty(PMRootDomain, 
                                    CFSTR(kIOPMSettingWakeOnRingKey), 
                                    (p->fWakeOnRing?number1:number0));
    }
    
    // Automatic Restart On Power Loss, aka FileServer mode
    if(true == IOPMFeatureIsAvailable(CFSTR(kIOPMRestartOnPowerLossKey), providing_power))
    {
        ret = IORegistryEntrySetCFProperty(PMRootDomain, 
                                    CFSTR(kIOPMSettingRestartOnPowerLossKey), 
                                    (p->fAutomaticRestart?number1:number0));
    }
    
    // Wake on change of AC state -- battery to AC or vice versa
    if(true == IOPMFeatureIsAvailable(CFSTR(kIOPMWakeOnACChangeKey), providing_power))
    {
        ret = IORegistryEntrySetCFProperty(PMRootDomain, 
                                    CFSTR(kIOPMSettingWakeOnACChangeKey), 
                                    (p->fWakeOnACChange?number1:number0));
    }
    
    // Disable power button sleep on PowerMacs, Cubes, and iMacs
    // Default is false == power button causes sleep
    if(true == IOPMFeatureIsAvailable(CFSTR(kIOPMSleepOnPowerButtonKey), providing_power))
    {
        ret = IORegistryEntrySetCFProperty(PMRootDomain, 
                    CFSTR(kIOPMSettingSleepOnPowerButtonKey), 
                    (p->fSleepOnPowerButton?kCFBooleanFalse:kCFBooleanTrue));
    }    
    
    // Wakeup on clamshell open
    // Default is true == wakeup when the clamshell opens
    if(true == IOPMFeatureIsAvailable(CFSTR(kIOPMWakeOnClamshellKey), providing_power))
    {
        ret = IORegistryEntrySetCFProperty(PMRootDomain, 
                                    CFSTR(kIOPMSettingWakeOnClamshellKey), 
                                    (p->fWakeOnClamshell?number1:number0));            
    }

    // Mobile Motion Module
    // Defaults to on
    if(true == IOPMFeatureIsAvailable(CFSTR(kIOPMMobileMotionModuleKey), providing_power))
    {
        type = 7;   // kPMMotionSensor defined in Tiger
        IOPMSetAggressiveness(PM_connection, type, p->fMobileMotionModule);
    }


    CFDictionaryRef dict = NULL;
    if((dict = CFDictionaryGetValue(System, prof)) )
    {
	ProcessHibernateSettings(dict, PMRootDomain);
    }

    /* PowerStep and Reduce Processor Speed are handled by a separate configd 
       plugin that's watching the SCDynamicStore key 
       State:/IOKit/PowerManagement/CurrentSettings. Changes to the settings 
       notify the configd plugin, which then activates th processor speed 
       settings. Note that IOPMActivatePMPreference updates that key in the 
       SCDynamicStore when we activate new settings. 
       See DynamicPowerStep configd plugin.

       A separate display manager process handles activating the 
       Reduce Brightness key through the same mechanism desribed above for 
       Reduce Process & Dynamic Power Step.
    */
    CFRelease(number0);
    CFRelease(number1);
    if(power_source_info) CFRelease(power_source_info);
    IOServiceClose(PM_connection);
    IOObjectRelease(PMRootDomain);
    return 0;
}

static void GetAggressivenessValue(
    CFTypeRef           obj,
    CFNumberType        type,
    unsigned int        *ret)
{
    *ret = 0;
    if (isA_CFNumber(obj))
    {            
        CFNumberGetValue(obj, type, ret);
        return;
    } 
    else if (isA_CFBoolean(obj))
    {
        *ret = CFBooleanGetValue(obj);
        return;
    }
}

/* For internal use only */
static int getAggressivenessFactorsFromProfile(
    CFDictionaryRef System, 
    CFStringRef prof, 
    IOPMAggressivenessFactors *agg)
{
    CFDictionaryRef p = NULL;

    if( !(p = CFDictionaryGetValue(System, prof)) )
    {
        return -1;
    }

    if(!agg) return -1;
    
    /*
     * Extract battery settings into s->battery
     */
    
    // dim
    GetAggressivenessValue(CFDictionaryGetValue(p, CFSTR(kIOPMDisplaySleepKey)),
                           kCFNumberSInt32Type, &agg->fMinutesToDim);
    
    // spin down
    GetAggressivenessValue(CFDictionaryGetValue(p, CFSTR(kIOPMDiskSleepKey)),
                           kCFNumberSInt32Type, &agg->fMinutesToSpin);

    // sleep
    GetAggressivenessValue(CFDictionaryGetValue(p, CFSTR(kIOPMSystemSleepKey)),
                           kCFNumberSInt32Type, &agg->fMinutesToSleep);

    // Wake On Magic Packet
    GetAggressivenessValue(CFDictionaryGetValue(p, CFSTR(kIOPMWakeOnLANKey)),
                           kCFNumberSInt32Type, &agg->fWakeOnLAN);

    // Wake On Ring
    GetAggressivenessValue(CFDictionaryGetValue(p, CFSTR(kIOPMWakeOnRingKey)),
                           kCFNumberSInt32Type, &agg->fWakeOnRing);

    // AutomaticRestartOnPowerLoss
    GetAggressivenessValue(CFDictionaryGetValue(p, CFSTR(kIOPMRestartOnPowerLossKey)),
                           kCFNumberSInt32Type, &agg->fAutomaticRestart);
    
    // Disable Power Button Sleep
    GetAggressivenessValue(CFDictionaryGetValue(p, CFSTR(kIOPMSleepOnPowerButtonKey)),
                           kCFNumberSInt32Type, &agg->fSleepOnPowerButton);    

    // Disable Clamshell Wakeup
    GetAggressivenessValue(CFDictionaryGetValue(p, CFSTR(kIOPMWakeOnClamshellKey)),
                           kCFNumberSInt32Type, &agg->fWakeOnClamshell);    

    // Wake on AC Change
    GetAggressivenessValue(CFDictionaryGetValue(p, CFSTR(kIOPMWakeOnACChangeKey)),
                           kCFNumberSInt32Type, &agg->fWakeOnACChange);    

    // Disable intermediate dimming stage for display sleep
    GetAggressivenessValue(CFDictionaryGetValue(p, CFSTR(kIOPMDisplaySleepUsesDimKey)),
                           kCFNumberSInt32Type, &agg->fDisplaySleepUsesDimming);    

    // MMM
    GetAggressivenessValue(CFDictionaryGetValue(p, CFSTR(kIOPMMobileMotionModuleKey)),
                           kCFNumberSInt32Type, &agg->fMobileMotionModule);    

    return 0;
}

/* Maps a PowerManagement string constant
 *   -> to its corresponding Supported Feature in IOPMrootDomain
 */
static CFStringRef
supportedNameForPMName( CFStringRef pm_name )
{
    if(CFEqual(pm_name, CFSTR(kIOPMDisplaySleepUsesDimKey)))
    {
        return CFSTR("DisplayDims");
    }

    if(CFEqual(pm_name, CFSTR(kIOPMWakeOnLANKey)))
    {
        return CFSTR("WakeOnMagicPacket");
    }

    if(CFEqual(pm_name, CFSTR(kIOPMMobileMotionModuleKey)))
    {
        return CFSTR("MobileMotionModule");
    }

    if( CFEqual(pm_name, CFSTR(kIOHibernateModeKey))
        || CFEqual(pm_name, CFSTR(kIOHibernateFreeRatioKey))
        || CFEqual(pm_name, CFSTR(kIOHibernateFreeTimeKey))
        || CFEqual(pm_name, CFSTR(kIOHibernateFileKey)))
    {
        return CFSTR(kIOHibernateFeatureKey);
    }

    if( CFEqual(pm_name, CFSTR(kIOPMReduceSpeedKey))
        || CFEqual(pm_name, CFSTR(kIOPMDynamicPowerStepKey)) 
        || CFEqual(pm_name, CFSTR(kIOPMWakeOnRingKey))
        || CFEqual(pm_name, CFSTR(kIOPMRestartOnPowerLossKey))
        || CFEqual(pm_name, CFSTR(kIOPMWakeOnACChangeKey))
        || CFEqual(pm_name, CFSTR(kIOPMWakeOnClamshellKey)) 
        || CFEqual(pm_name, CFSTR(kIOPMReduceBrightnessKey)) )
    {
        return pm_name;
    }

    return NULL;
}

// Helper for IOPMFeatureIsAvailable
static bool
featureSupportsPowerSource(CFTypeRef featureDetails, CFStringRef power_source)
{
    CFNumberRef         featureNum   = NULL;
    CFNumberRef         tempNum      = NULL;
    CFArrayRef          featureArr   = NULL;
    uint32_t            ps_support   = 0;
    uint32_t            tmp;
    unsigned int        i;
    
    if(!power_source) {
        // Lack of a defined power source just gets a "true" return
        // if the setting is supported on ANY power source.
        return true;
    }
    
    if( (featureNum = isA_CFNumber(featureDetails)) )
    {
        CFNumberGetValue(featureNum, kCFNumberSInt32Type, &ps_support);
    } else if( (featureArr = isA_CFArray(featureDetails)) )
    {
        // If several entitites are asserting a given feature, we OR
        // together their supported power sources.

        unsigned int arrayCount = CFArrayGetCount(featureArr);
        for(i = 0; i<arrayCount; i++)
        {
            tempNum = isA_CFNumber(CFArrayGetValueAtIndex(featureArr, i));
            if(tempNum) {
                CFNumberGetValue(tempNum, kCFNumberSInt32Type, &tmp);
                ps_support |= tmp;
            }
        }
    }

    if(CFEqual(CFSTR(kIOPMACPowerKey), power_source) )
    {
        return (ps_support & kIOPMSupportedOnAC) ? true : false;
    } else if(CFEqual(CFSTR(kIOPMBatteryPowerKey), power_source) )
    {
        return (ps_support & kIOPMSupportedOnBatt) ? true : false;
    } else if(CFEqual(CFSTR(kIOPMUPSPowerKey), power_source) )
    {
        return (ps_support & kIOPMSupportedOnUPS) ? true : false;
    } else {
        // unexpected power source argument
        return false;
    }

}

/*** IOPMFeatureIsAvailable
     Arguments-
        CFStringRef PMFeature - Name of a PM feature
                (like "WakeOnRing" or "Reduce Processor Speed")
        CFStringRef power_source - The current power source 
                  (like "AC Power" or "Battery Power")
     Return value-
        true if the given PM feature is supported on the given power source
        false if the feature is unsupported
 ***/
bool IOPMFeatureIsAvailable(CFStringRef PMFeature, CFStringRef power_source)
{
    CFDictionaryRef		        supportedFeatures = NULL;
    CFStringRef                 supportedString = NULL;
    CFTypeRef                   featureDetails = NULL;
    CFArrayRef                  tmp_array = NULL;

    io_registry_entry_t		    registry_entry = MACH_PORT_NULL;
    bool                        ret = false;

    registry_entry = getPMRootDomainRef();
    if(!registry_entry) goto exit;
    
    supportedFeatures = IORegistryEntryCreateCFProperty(
                registry_entry, CFSTR("Supported Features"),
                kCFAllocatorDefault, kNilOptions);
    IOObjectRelease(registry_entry);
    
    if(CFEqual(PMFeature, CFSTR(kIOPMDisplaySleepKey))
        || CFEqual(PMFeature, CFSTR(kIOPMSystemSleepKey))
        || CFEqual(PMFeature, CFSTR(kIOPMDiskSleepKey)))
    {
        ret = true;
        goto exit;
    }

// *********************************
// Special case for PowerButtonSleep    

    if(CFEqual(PMFeature, CFSTR(kIOPMSleepOnPowerButtonKey)))
    {
        // Pressing the power button only causes sleep on desktop PowerMacs, 
        // cubes, and iMacs.
        // Therefore this feature is not supported on portables.
        // We'll use the presence of a battery (or the capability for a battery) 
        // as evidence whether this is a portable or not.
        IOReturn r = IOPMCopyBatteryInfo(kIOMasterPortDefault, &tmp_array);
        if((r == kIOReturnSuccess) && tmp_array) 
        {
            CFRelease(tmp_array);
            ret = false;
        } else ret = true;        
        goto exit;
    }
    
// *********************************
// Special case for ReduceBrightness
    
    if ( CFEqual(PMFeature, CFSTR(kIOPMReduceBrightnessKey)) )
    {
        // ReduceBrightness feature is only supported on laptops
        // and on desktops with UPS with brightness-adjustable LCD displays.
        // These machines report a "DisplayDims" property in the 
        // supportedFeatures dictionary.
        // ReduceBrightness is never supported on AC Power.
        CFTypeRef ps = IOPSCopyPowerSourcesInfo();
        if( ps 
            && ( IOPSGetActiveBattery(ps) || IOPSGetActiveUPS(ps) ) 
            && supportedFeatures
            && CFDictionaryGetValue(supportedFeatures, CFSTR("DisplayDims"))
            && !CFEqual(power_source, CFSTR(kIOPMACPowerKey)) )
        {
            ret = true;
        } else {
            ret = false;
        }

        if(ps) CFRelease(ps);
        goto exit;
    }

// ***********************************
// Generic code for all other settings    
    
    if(!supportedFeatures) {
        ret = false;
        goto exit;
    }
    
    supportedString = supportedNameForPMName( PMFeature );
    if(!supportedString) {
        ret = false;
        goto exit;
    }
    
    featureDetails = CFDictionaryGetValue(supportedFeatures, supportedString);
    if(!featureDetails) {
        ret = false;
        goto exit;
    }
    
    if(featureSupportsPowerSource(featureDetails, power_source))
    {
        ret = true;    
    }
    

exit:
    if(supportedFeatures) CFRelease(supportedFeatures);
    return ret;
}

/***
 * removeIrrelevantPMProperties
 *
 * Prunes unsupported properties from the energy dictionary.
 * e.g. If your machine doesn't have a modem, this removes the Wake On Ring property.
 ***/
static void IOPMRemoveIrrelevantProperties(CFMutableDictionaryRef energyPrefs)
{
    int                         profile_count = 0;
    int                         dict_count = 0;
    CFStringRef                 *profile_keys = NULL;
    CFDictionaryRef             *profile_vals = NULL;
    CFStringRef                 *dict_keys    = NULL;
    CFDictionaryRef             *dict_vals    = NULL;
    CFMutableDictionaryRef      this_profile;
    CFTypeRef                   ps_snapshot;
    
    ps_snapshot = IOPSCopyPowerSourcesInfo();
    
    /*
     * Remove features when not supported - 
     *      Wake On Administrative Access, Dynamic Speed Step, etc.
     */
    profile_count = CFDictionaryGetCount(energyPrefs);
    profile_keys = (CFStringRef *)malloc(sizeof(CFStringRef) * profile_count);
    profile_vals = (CFDictionaryRef *)malloc(sizeof(CFDictionaryRef) * profile_count);
    if(!profile_keys || !profile_vals) return;
    
    CFDictionaryGetKeysAndValues(energyPrefs, (const void **)profile_keys, (const void **)profile_vals);
    // For each CFDictionary at the top level (battery, AC)
    while(--profile_count >= 0)
    {
        if(kCFBooleanTrue != IOPSPowerSourceSupported(ps_snapshot, profile_keys[profile_count]))
        {
            // Remove dictionary if the whole power source isn't supported on this machine.
            CFDictionaryRemoveValue(energyPrefs, profile_keys[profile_count]);        
        } else {
        
            // Make a mutable copy of the prefs dictionary

            this_profile = (CFMutableDictionaryRef)isA_CFDictionary(
                CFDictionaryGetValue(energyPrefs, profile_keys[profile_count]));
            if(!this_profile) continue;

            this_profile = CFDictionaryCreateMutableCopy(NULL, 0, this_profile);
            if(!this_profile) continue;

            CFDictionarySetValue(energyPrefs, profile_keys[profile_count], this_profile);
            CFRelease(this_profile);

            // And prune unneeded settings from our new mutable property            

            dict_count = CFDictionaryGetCount(this_profile);
            dict_keys = (CFStringRef *)malloc(sizeof(CFStringRef) * dict_count);
            dict_vals = (CFDictionaryRef *)malloc(sizeof(CFDictionaryRef) * dict_count);
            if(!dict_keys || !dict_vals) continue;
            CFDictionaryGetKeysAndValues(this_profile, 
                        (const void **)dict_keys, (const void **)dict_vals);
            // For each specific property within each dictionary
            while(--dict_count >= 0)
            {
                if( !IOPMFeatureIsAvailable((CFStringRef)dict_keys[dict_count], 
                                    (CFStringRef)profile_keys[profile_count]) )
                {
                    // If the property isn't supported, remove it
                    CFDictionaryRemoveValue(this_profile, (CFStringRef)dict_keys[dict_count]);    
                }
            }
            free(dict_keys);
            free(dict_vals);
        }
    }

    free(profile_keys);
    free(profile_vals);
    if(ps_snapshot) CFRelease(ps_snapshot);
    return;
}

/***
 * getCheetahPumaEnergySettings
 *
 * Reads the old Energy Saver preferences file from /Library/Preferences/com.apple.PowerManagement.xml
 *
 ***/
static int getCheetahPumaEnergySettings(CFMutableDictionaryRef energyPrefs)
{
   SCPreferencesRef             CheetahPrefs = NULL;
   CFMutableDictionaryRef       s = NULL;
   CFNumberRef                  n;
   CFBooleanRef                 b;
   
   if(!energyPrefs) return 0;
   CheetahPrefs = SCPreferencesCreate (kCFAllocatorDefault, 
                                     CFSTR("I/O Kit PM Library"),
                                     CFSTR("/Library/Preferences/com.apple.PowerManagement.plist"));
    
    if(!CheetahPrefs) return 0;
    
    s = (CFMutableDictionaryRef)CFDictionaryGetValue(energyPrefs, CFSTR(kIOPMBatteryPowerKey));
    if(!s)
    {
        CFRelease(CheetahPrefs);
        return 0;
    }
    n = (CFNumberRef)SCPreferencesGetValue(CheetahPrefs, kCheetahDimKey);
    if(n) CFDictionaryAddValue(s, CFSTR(kIOPMDisplaySleepKey), n);
    n = (CFNumberRef)SCPreferencesGetValue(CheetahPrefs, kCheetahDiskKey);
    if(n) CFDictionaryAddValue(s, CFSTR(kIOPMDiskSleepKey), n);
    n = (CFNumberRef)SCPreferencesGetValue(CheetahPrefs, kCheetahSleepKey);
    if(n) CFDictionaryAddValue(s, CFSTR(kIOPMSystemSleepKey), n);
    b = (CFBooleanRef)SCPreferencesGetValue(CheetahPrefs, kCheetahRestartOnPowerLossKey);
    if(b) CFDictionaryAddValue(s, CFSTR(kIOPMRestartOnPowerLossKey), b);
    b = (CFBooleanRef)SCPreferencesGetValue(CheetahPrefs, kCheetahWakeForNetworkAccessKey);
    if(b) CFDictionaryAddValue(s, CFSTR(kIOPMWakeOnLANKey), b);
    b = (CFBooleanRef)SCPreferencesGetValue(CheetahPrefs, kCheetahWakeOnRingKey);
    if(b) CFDictionaryAddValue(s, CFSTR(kIOPMWakeOnRingKey), b);
                    

    s = (CFMutableDictionaryRef)CFDictionaryGetValue(energyPrefs, CFSTR(kIOPMACPowerKey));
    if(!s)
    {
        CFRelease(CheetahPrefs);
        return 0;
    }
    n = (CFNumberRef)SCPreferencesGetValue(CheetahPrefs, kCheetahDimKey);
    if(n) CFDictionaryAddValue(s, CFSTR(kIOPMDisplaySleepKey), n);
    n = (CFNumberRef)SCPreferencesGetValue(CheetahPrefs, kCheetahDiskKey);
    if(n) CFDictionaryAddValue(s, CFSTR(kIOPMDiskSleepKey), n);
    n = (CFNumberRef)SCPreferencesGetValue(CheetahPrefs, kCheetahSleepKey);
    if(n) CFDictionaryAddValue(s, CFSTR(kIOPMSystemSleepKey), n);
    b = (CFBooleanRef)SCPreferencesGetValue(CheetahPrefs, kCheetahRestartOnPowerLossKey);
    if(b) CFDictionaryAddValue(s, CFSTR(kIOPMRestartOnPowerLossKey), b);
    b = (CFBooleanRef)SCPreferencesGetValue(CheetahPrefs, kCheetahWakeForNetworkAccessKey);
    if(b) CFDictionaryAddValue(s, CFSTR(kIOPMWakeOnLANKey), b);
    b = (CFBooleanRef)SCPreferencesGetValue(CheetahPrefs, kCheetahWakeOnRingKey);
    if(b) CFDictionaryAddValue(s, CFSTR(kIOPMWakeOnRingKey), b);


    CFRelease(CheetahPrefs);

     return 1; // success
}


/**************************************************
*
* Energy Saver Preferences
*
**************************************************/

CFMutableDictionaryRef IOPMCopyPMPreferences(void)
{
    CFMutableDictionaryRef                  energyDict = NULL;
    CFDictionaryRef                         tmp_dict = NULL;
    SCPreferencesRef                        energyPrefs = NULL;
    CFDictionaryRef                         tmp = NULL;
    CFMutableDictionaryRef                  batterySettings = NULL;
    CFMutableDictionaryRef                  ACSettings = NULL;
    CFMutableDictionaryRef                  UPSSettings = NULL;
    bool                                    usingDefaults = true;

    energyPrefs = SCPreferencesCreate( kCFAllocatorDefault, kIOPMAppName, kIOPMPrefsPath );
    if(!energyPrefs) {
        return NULL;
    }
    
    // Attempt to read battery & AC settings
    tmp_dict = isA_CFDictionary(SCPreferencesGetValue(energyPrefs, CFSTR("Custom Profile")));
    
    // If com.apple.PowerManagement.xml opened correctly, read data from it
    if(tmp_dict)
    {
        usingDefaults = false;
        
        // Tiger preferences file format
        energyDict = CFDictionaryCreateMutableCopy(
            kCFAllocatorDefault,
            0,
            tmp_dict);
        if(!energyDict) goto failure_exit;
    } else {
        // Try Panther/Jaguar  prefs formats

        batterySettings = (CFMutableDictionaryRef)isA_CFDictionary(
                SCPreferencesGetValue(energyPrefs, CFSTR(kIOPMBatteryPowerKey)));
        ACSettings = (CFMutableDictionaryRef)isA_CFDictionary(
                SCPreferencesGetValue(energyPrefs, CFSTR(kIOPMACPowerKey)));
        UPSSettings = (CFMutableDictionaryRef)isA_CFDictionary(
                SCPreferencesGetValue(energyPrefs, CFSTR(kIOPMUPSPowerKey)));

        if(batterySettings || ACSettings || UPSSettings) usingDefaults = false;

        energyDict = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 
            0, 
            &kCFTypeDictionaryKeyCallBacks, 
            &kCFTypeDictionaryValueCallBacks);
        if(!energyDict) goto failure_exit;
        
        if(batterySettings)
            CFDictionaryAddValue(energyDict, CFSTR(kIOPMBatteryPowerKey), batterySettings);
        if(ACSettings)
            CFDictionaryAddValue(energyDict, CFSTR(kIOPMACPowerKey), ACSettings);
        if(UPSSettings)
            CFDictionaryAddValue(energyDict, CFSTR(kIOPMUPSPowerKey), UPSSettings);
    }
    
    // Make sure that the enclosed dictionaries are all mutable
    tmp = isA_CFDictionary(CFDictionaryGetValue(energyDict, CFSTR(kIOPMBatteryPowerKey)));
    if(tmp) batterySettings = CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, tmp);
    else batterySettings = CFDictionaryCreateMutable(0, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if(batterySettings)
    {
        CFDictionarySetValue(energyDict, CFSTR(kIOPMBatteryPowerKey), batterySettings);
        CFRelease(batterySettings);
    } else goto failure_exit;

    tmp = isA_CFDictionary(CFDictionaryGetValue(energyDict, CFSTR(kIOPMACPowerKey)));
    if(tmp) ACSettings = CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, tmp);
    else ACSettings = CFDictionaryCreateMutable(0, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if(ACSettings) {
        CFDictionarySetValue(energyDict, CFSTR(kIOPMACPowerKey), ACSettings);
        CFRelease(ACSettings);
    } else goto failure_exit;

    tmp = isA_CFDictionary(CFDictionaryGetValue(energyDict, CFSTR(kIOPMUPSPowerKey)));
    if(tmp) UPSSettings = CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, tmp);        
    else UPSSettings = CFDictionaryCreateMutable(0, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if(UPSSettings) {
        CFDictionarySetValue(energyDict, CFSTR(kIOPMUPSPowerKey), UPSSettings);
        CFRelease(UPSSettings);
    } else goto failure_exit;
    
    // INVARIANT: At this point we want a mutable dictionary energyDict
    // containing 3 mutable preferences dictionaries that are either empty or contain some settings.

    // Check for existence of Puma/Cheetah prefs format
    // And add settings defined there if present
    getCheetahPumaEnergySettings(energyDict);

    // Fill in any undefined settings with our defaults
    // i.e. if no current or legacy prefs files exist, getDefaultEnergySettings()
    // completely populates the default EnergySaver preferences.
    getDefaultEnergySettings(energyDict);

    // Remove any unsupported key/value pairs (including some of 
    // those we just added in getDefaultEnergySettings)    
    IOPMRemoveIrrelevantProperties(energyDict);
    
    if(usingDefaults) {
        // If we couldn't find any user-specified settings on disk, tag this dictionary as
        // "Defaults" so that BatteryMonitor and EnergySaver can tell these are user-selected
        // values or just the system defaults.
        CFDictionarySetValue(energyDict, CFSTR(kIOPMDefaultPreferencesKey), kCFBooleanTrue);
    }
  
//normal_exit:
    if(energyPrefs) CFRelease(energyPrefs);      
    return energyDict;
    
failure_exit:
    if(energyPrefs) CFRelease(energyPrefs);
    if(energyDict) CFRelease(energyDict);
    return 0;
}

// TODO: Migrate this into PM daemon
IOReturn IOPMActivatePMPreference(CFDictionaryRef SystemProfiles, CFStringRef profile)
{
    IOPMAggressivenessFactors       *agg = NULL;
    CFDictionaryRef                 activePMPrefs = NULL;
    CFDictionaryRef                 newPMPrefs = NULL;
    SCDynamicStoreRef               dynamic_store = NULL;

    if(0 == isA_CFDictionary(SystemProfiles) || 0 == isA_CFString(profile)) {
        return kIOReturnBadArgument;
    }

    // Activate settings by sending them to the kernel
    agg = (IOPMAggressivenessFactors *)malloc(sizeof(IOPMAggressivenessFactors));
    getAggressivenessFactorsFromProfile(SystemProfiles, profile, agg);
    sendEnergySettingsToKernel(SystemProfiles, profile, agg);
    free(agg);
    
    // Put the new settings in the SCDynamicStore for interested apps
    dynamic_store = SCDynamicStoreCreate(kCFAllocatorDefault,
                                         CFSTR("IOKit User Library"),
                                         NULL, NULL);
    if(dynamic_store == NULL) return kIOReturnError;

    activePMPrefs = isA_CFDictionary(SCDynamicStoreCopyValue(dynamic_store,  
                                         CFSTR(kIOPMDynamicStoreSettingsKey)));

    newPMPrefs = isA_CFDictionary(CFDictionaryGetValue(SystemProfiles, profile));

    // If there isn't currently a value for kIOPMDynamicStoreSettingsKey
    //    or the current value is different than the new value
    if( !activePMPrefs || (newPMPrefs && !CFEqual(activePMPrefs, newPMPrefs)) )
    {
        // Then set the kIOPMDynamicStoreSettingsKey to the new value
        SCDynamicStoreSetValue(dynamic_store,  
                               CFSTR(kIOPMDynamicStoreSettingsKey),
                               newPMPrefs);
    }
    
    if(activePMPrefs) CFRelease(activePMPrefs);
    CFRelease(dynamic_store);
    return kIOReturnSuccess;
}

// Sets (and activates) Custom power profile
IOReturn IOPMSetPMPreferences(CFDictionaryRef ESPrefs)
{
    IOReturn                    ret = kIOReturnError;
    SCPreferencesRef            energyPrefs = NULL;
    
    energyPrefs = SCPreferencesCreate( kCFAllocatorDefault, kIOPMAppName, kIOPMPrefsPath );
    if(!energyPrefs) return kIOReturnError;
    
    if(!SCPreferencesLock(energyPrefs, true))
    {  
        // handle error
        if(kSCStatusAccessError == SCError()) ret = kIOReturnNotPrivileged;
        else ret = kIOReturnError;
        goto exit;
    }

    if(!SCPreferencesSetValue(energyPrefs, CFSTR("Custom Profile"), ESPrefs))
    {
        ret = kIOReturnError;
        goto exit;
    }

    // If older profiles exist, remove them in favor of the new format
    SCPreferencesRemoveValue(energyPrefs, CFSTR(kIOPMACPowerKey));
    SCPreferencesRemoveValue(energyPrefs, CFSTR(kIOPMBatteryPowerKey));
    SCPreferencesRemoveValue(energyPrefs, CFSTR(kIOPMUPSPowerKey));

    if(!SCPreferencesCommitChanges(energyPrefs))
    {
        // handle error
        if(kSCStatusAccessError == SCError()) ret = kIOReturnNotPrivileged;
        else ret = kIOReturnError;
        goto exit;
    }
    
    if(!SCPreferencesApplyChanges(energyPrefs))
    {
        // handle error
        if(kSCStatusAccessError == SCError()) ret = kIOReturnNotPrivileged;
        else ret = kIOReturnError;        
        goto exit;
    }
    ret = kIOReturnSuccess;
exit:
    if(energyPrefs) {
        SCPreferencesUnlock(energyPrefs);
        CFRelease(energyPrefs);
    }
    return ret;
}

/**************************************************
*
* Power Profiles
*
*
**************************************************/

static void mergeDictIntoMutable(
    CFMutableDictionaryRef  target,
    CFDictionaryRef         overrides)
{
    const CFStringRef         *keys;
    const CFTypeRef           *objs;
    int                 count;
    int                 i;
    
    count = CFDictionaryGetCount(overrides);
    if(0 == count) return;

    keys = (CFStringRef *)malloc(sizeof(CFStringRef) * count);
    objs = (CFTypeRef *)malloc(sizeof(CFTypeRef) * count);
    if(!keys || !objs) return;

    CFDictionaryGetKeysAndValues(overrides, 
                    (const void **)keys, (const void **)objs);
    for(i=0; i<count; i++)
    {
        CFDictionarySetValue(target, keys[i], objs[i]);    
    }
    free((void *)keys);
    free((void *)objs);    
}

/* _copySystemProvidedProfiles()
 *
 * The PlatformExpert kext on the system may conditionally override the Energy
 * Saver's profiles. Only the PlatformExpert should be setting these properties.
 *
 * We use two supported properties - kIOPMSystemDefaultOverrideKey & 
 * kIOPMSystemDefaultProfilesKey. We check first for 
 * kIOPMSystemDefaultOverrideKey (a partial settings defaults substitution), 
 * and if it's not present we'll use kIOPMSystemDefaultProfilesKey 
 * (a complete settings defaults substitution).
 *
 * Overrides are a single dictionary of PM settings merged into the 
 * default PM profiles found on the root volume, under the PM bundle, as
 * com.apple.SystemPowerProfileDefaults.plist
 *
 * Alternatively, Overrides are a 3 dictionary set, each dictionary
 * being a proper PM settings dictionary. The 3 keys must be
 * "AC Power", "Battery Power" and "UPS Power" respectively. Each
 * dictionary under those keys should contain only PM settings.
 * 
 * DefaultProfiles is a CFArray of size 5, containing CFDictionaries
 * which each contain 3 dictionaries in turn
 */
static CFArrayRef      _copySystemProvidedProfiles()
{
    io_registry_entry_t         registry_entry = MACH_PORT_NULL;
    CFTypeRef                   cftype_total_prof_override = NULL;
    CFTypeRef                   cftype_overrides = NULL;
    CFArrayRef                  retArray = NULL;
    CFDictionaryRef             overrides = NULL;
    CFDictionaryRef             ac_over = NULL;
    CFDictionaryRef             batt_over = NULL;
    CFDictionaryRef             ups_over = NULL;

    CFArrayRef                  sysPowerProfiles = NULL;
    CFMutableArrayRef           mArrProfs = NULL;
    int                         count = 0;
    int                         i = 0;
    
    registry_entry = IOServiceGetMatchingService(kIOMasterPortDefault, 
                        IOServiceNameMatching("IOPMrootDomain"));
    if(MACH_PORT_NULL == registry_entry) return NULL;
    
    /* O v e r r i d e */

    cftype_overrides = IORegistryEntryCreateCFProperty(registry_entry, 
                        CFSTR(kIOPMSystemDefaultOverrideKey),
                        kCFAllocatorDefault, 0);
    if( !(overrides = isA_CFDictionary(cftype_overrides)) ) {
        // Expect overrides to be a CFDictionary. If not, skip.
        if(cftype_overrides) {
            CFRelease(cftype_overrides); cftype_overrides = NULL;
        }
        goto TrySystemDefaultProfiles;
    }
    
    ac_over = CFDictionaryGetValue(overrides, CFSTR(kIOPMACPowerKey));
    batt_over = CFDictionaryGetValue(overrides, CFSTR(kIOPMBatteryPowerKey));
    ups_over = CFDictionaryGetValue(overrides, CFSTR(kIOPMUPSPowerKey));

    // Overrides either contains 3 dictionaries of PM Settings keyed as
    // AC, Battery, and UPS Power, or it is itself a dictionary of PM Settings.
    if(ac_over && batt_over && ups_over) 
    {
        // Good. All 3 power source settings types are represented.
        // Do nothing here.
    } else if(!ac_over && !batt_over && !ups_over) 
    {
        // The dictionary didn't specify any per-power source overrides, which
        // means that it's a flat dictionary strictly of PM settings.
        // We duplicate it 3 ways, as each overridden setting in this dictionary
        // will be applied to each power source's settings.
        ac_over = batt_over = ups_over = overrides;
    } else {
        // Bad form for overrides dictionary.
        goto TrySystemDefaultProfiles;
    }
    
    // ac_over, batt_over, ups_over now contain the PM settings to be merged
    // into the system's default profiles. The settings defined in ac_over,
    // batt_over, and ups_over, will override the system's defaults from file:
    //
    // com.apple.SystemPowerProfileDefaults.plist in PowerManagement.bundle
    
    
    sysPowerProfiles = _createDefaultSystemProfiles();
    if(!sysPowerProfiles) goto exit;
    count = CFArrayGetCount(sysPowerProfiles);

    mArrProfs = CFArrayCreateMutable(0, count, &kCFTypeArrayCallBacks);
    for(i=0; i<count; i++)
    {
        CFMutableDictionaryRef      mSettingsAC;
        CFMutableDictionaryRef      mSettingsBatt;
        CFMutableDictionaryRef      mSettingsUPS;
        CFMutableDictionaryRef      mProfile;
        CFDictionaryRef             _profile;
        CFDictionaryRef             tmp;

        _profile = (CFDictionaryRef)CFArrayGetValueAtIndex(sysPowerProfiles, i);
        if(!_profile) continue;

        // Create a new mutable profile to modify & override selected settings        
        mProfile = CFDictionaryCreateMutable(0, 
                        CFDictionaryGetCount(_profile), 
                        &kCFTypeDictionaryKeyCallBacks, 
                        &kCFTypeDictionaryValueCallBacks);
                        
        if(!mProfile) continue;        
        // Add new mutable profile to new mutable array of profiles
        CFArraySetValueAtIndex(mArrProfs, i, mProfile);
        CFRelease(mProfile);

        tmp = (CFDictionaryRef)CFDictionaryGetValue(_profile, 
                        CFSTR(kIOPMACPowerKey));
        if(!tmp) continue;
        mSettingsAC = CFDictionaryCreateMutableCopy(0, 
                        CFDictionaryGetCount(tmp), tmp);
                        

        tmp = (CFDictionaryRef)CFDictionaryGetValue(_profile, 
                        CFSTR(kIOPMBatteryPowerKey));
        if(!tmp) continue;
        mSettingsBatt = CFDictionaryCreateMutableCopy(0, 
                        CFDictionaryGetCount(tmp), tmp);

        tmp = (CFDictionaryRef)CFDictionaryGetValue(_profile, 
                        CFSTR(kIOPMUPSPowerKey));
        if(!tmp) continue;
        mSettingsUPS = CFDictionaryCreateMutableCopy(0, 
                        CFDictionaryGetCount(tmp), tmp);
        
        if( !(mSettingsAC && mSettingsBatt && mSettingsUPS) ) {
            if(sysPowerProfiles) { 
                CFRelease(sysPowerProfiles); sysPowerProfiles = NULL;
            }
            if(mSettingsAC) {
                CFRelease(mSettingsAC); mSettingsAC = NULL;
            }
            if(mSettingsBatt) {
                CFRelease(mSettingsBatt); mSettingsBatt = NULL;
            }
            if(mSettingsUPS) {
                CFRelease(mSettingsUPS); mSettingsUPS = NULL;
            }
            if(mArrProfs) {
                CFRelease(mArrProfs); mArrProfs = NULL;
            }
            goto TrySystemDefaultProfiles;
        }

        // Add these new mutable dictionaries to our new mutable profile
        
        CFDictionarySetValue(mProfile, 
                            CFSTR(kIOPMACPowerKey), 
                            mSettingsAC);
        
        CFDictionarySetValue(mProfile, 
                            CFSTR(kIOPMBatteryPowerKey), 
                            mSettingsBatt);

        CFDictionarySetValue(mProfile, 
                            CFSTR(kIOPMUPSPowerKey), 
                            mSettingsUPS);

        // And now... what we've all been waiting for... merge in the system
        // platform expert's provided default profiles.
        
        mergeDictIntoMutable(mSettingsAC, ac_over);
        mergeDictIntoMutable(mSettingsBatt, batt_over);
        mergeDictIntoMutable(mSettingsUPS, ups_over);

        // And release...

        CFRelease(mSettingsAC); mSettingsAC = NULL;
        CFRelease(mSettingsBatt); mSettingsBatt = NULL;
        CFRelease(mSettingsUPS); mSettingsUPS = NULL;
    }

    // Currently holding one retain on mArrProfs
    retArray = (CFArrayRef)mArrProfs;
    
    goto exit;

TrySystemDefaultProfiles:

    /* D e f a u l t   P r o f i l e s */

    // If there were no override PM settings, we check for a complete
    // power profiles definition instead. If so, return the CFArray
    // it contains wholesale.

    cftype_total_prof_override = IORegistryEntryCreateCFProperty(registry_entry, 
                        CFSTR(kIOPMSystemDefaultProfilesKey),
                        kCFAllocatorDefault, 0);
    if( isA_CFArray(cftype_total_prof_override) ) {
        retArray = (CFArrayRef)cftype_total_prof_override;
        goto exit;
    } else {
        if(cftype_total_prof_override) {
            CFRelease(cftype_total_prof_override);
            cftype_total_prof_override = NULL;
        }
    }

exit:
    if(sysPowerProfiles) { 
        CFRelease(sysPowerProfiles); sysPowerProfiles = NULL; 
    }
    if(cftype_overrides) {
        CFRelease(cftype_overrides); cftype_overrides = NULL;
    }
    IOObjectRelease(registry_entry);
    return retArray;
}

static CFArrayRef       _createDefaultSystemProfiles()
{
    CFURLRef                pm_bundle_url = 0;
    CFBundleRef             pm_bundle = 0;
    CFURLRef                profiles_url = 0;
    CFStringRef             profiles_path = 0;
    CFArrayRef              system_default_profiles = 0;
    CFArrayRef              return_array = 0;
    SCPreferencesRef        open_file = 0;
    
    pm_bundle_url = CFURLCreateWithFileSystemPath(
        kCFAllocatorDefault,
        CFSTR("/System/Library/SystemConfiguration/PowerManagement.bundle"),
        kCFURLPOSIXPathStyle,
        1);
    if(!pm_bundle_url) {
        goto exit;
    }

    pm_bundle = CFBundleCreate(
        kCFAllocatorDefault, 
        pm_bundle_url);
    if(!pm_bundle) {
        //syslog(LOG_INFO, "PM Preferences cannot locate PowerManagement.bundle\n");
        goto exit;
    }

    profiles_url = CFBundleCopyResourceURL(
        pm_bundle,
        CFSTR("com.apple.SystemPowerProfileDefaults.plist"),
        NULL,
        NULL);
    if(!profiles_url) {
        //syslog(LOG_INFO, "Can't find path to profiles\n");
        goto exit;
    }
    profiles_path = CFURLCopyPath(profiles_url);
    
    open_file = SCPreferencesCreate(
        kCFAllocatorDefault,
        CFSTR("PowerManagementPreferencse"),
        profiles_path);
    if(!open_file) {
        //syslog(LOG_INFO, "PM could not open System Profile defaults\n");
        goto exit;
    }    

    system_default_profiles = SCPreferencesGetValue(
        open_file,
        CFSTR("SystemProfileDefaults"));
    if(!isA_CFArray(system_default_profiles)) {
        //syslog(LOG_INFO, "Badly formatted or non-existent preferences on disk (0x%08x).\n", system_default_profiles);
        goto exit;
    }
    
    return_array = CFArrayCreateCopy(kCFAllocatorDefault, system_default_profiles);
    
exit:
    if(pm_bundle_url)       CFRelease(pm_bundle_url);
    if(pm_bundle)           CFRelease(pm_bundle);
    if(profiles_url)        CFRelease(profiles_url);
    if(profiles_path)       CFRelease(profiles_path);
    if(open_file)           CFRelease(open_file);
    
    if(!return_array) {
        syslog(LOG_INFO, "Power Management error: unable to load default System Power Profiles.\n");
    }    
    return return_array;
}

static CFDictionaryRef      _createDefaultProfileSelections(void)
{
    CFURLRef                pm_bundle_url = 0;
    CFBundleRef             pm_bundle = 0;
    CFURLRef                profiles_url = 0;
    CFStringRef             profiles_path = 0;
    CFDictionaryRef         default_profiles_selection = 0;
    CFDictionaryRef         return_dict = 0;
    SCPreferencesRef        open_file = 0;
    
    pm_bundle_url = CFURLCreateWithFileSystemPath(
        kCFAllocatorDefault,
        CFSTR("/System/Library/SystemConfiguration/PowerManagement.bundle"),
        kCFURLPOSIXPathStyle,
        1);
    if(!pm_bundle_url) {
        goto exit;
    }

    pm_bundle = CFBundleCreate(
        kCFAllocatorDefault, 
        pm_bundle_url);
    if(!pm_bundle) {
        //syslog(LOG_INFO, "PM Preferences cannot locate PowerManagement.bundle\n");
        goto exit;
    }

    profiles_url = CFBundleCopyResourceURL(
        pm_bundle,
        CFSTR("com.apple.SystemPowerProfileDefaults.plist"),
        NULL,
        NULL);
    if(!profiles_url) {
        //syslog(LOG_INFO, "Can't find path to profiles\n");
        goto exit;
    }
    profiles_path = CFURLCopyPath(profiles_url);
    
    open_file = SCPreferencesCreate(
        kCFAllocatorDefault,
        CFSTR("PowerManagementPreferences"),
        profiles_path);
    if(!open_file) {
        //syslog(LOG_INFO, "PM could not open System Profile defaults\n");
        goto exit;
    }    

    default_profiles_selection = SCPreferencesGetValue(
        open_file,
        CFSTR("DefaultProfileChoices"));
    if(!isA_CFDictionary(default_profiles_selection)) {
        //syslog(LOG_INFO, "Badly formatted or non-existent preferences on disk (0x%08x).\n", system_default_profiles);
        goto exit;
    }
    
    return_dict = CFDictionaryCreateCopy(kCFAllocatorDefault, default_profiles_selection);
    
exit:
    if(pm_bundle_url)       CFRelease(pm_bundle_url);
    if(pm_bundle)           CFRelease(pm_bundle);
    if(profiles_url)        CFRelease(profiles_url);
    if(profiles_path)       CFRelease(profiles_path);
    if(open_file)           CFRelease(open_file);
    
    if(!return_dict) {
        syslog(LOG_INFO, "Power Management error: unable to load default profiles selections.\n");
    }    
    return return_dict;
}

static CFDictionaryRef _createAllCustomProfileSelections(void)
{
    int                         j = -1;
    CFNumberRef                 n;
    CFMutableDictionaryRef      custom_dict = NULL;

    custom_dict = CFDictionaryCreateMutable(kCFAllocatorDefault, 3, 
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &j);
    if(!custom_dict || !n) return NULL;
    
    CFDictionarySetValue(custom_dict, CFSTR(kIOPMACPowerKey), n);
    CFDictionarySetValue(custom_dict, CFSTR(kIOPMBatteryPowerKey), n);
    CFDictionarySetValue(custom_dict, CFSTR(kIOPMUPSPowerKey), n);

    CFRelease(n);
    return custom_dict;
}


CFArrayRef          IOPMCopyPowerProfiles(void)
{
    CFArrayRef                      power_profiles = 0;
    CFMutableArrayRef               mutable_power_profiles = 0;
    CFDictionaryRef                 tmp;
    CFMutableDictionaryRef          mutable_profile;
    int                             i, p_count;

    // Provide the platform expert driver a chance to define better default
    // power settings for the machine this code is running on.
    power_profiles = _copySystemProvidedProfiles();
    if(!power_profiles) {
        power_profiles = _createDefaultSystemProfiles();
    }
    if(!power_profiles) return NULL;

    mutable_power_profiles = CFArrayCreateMutableCopy(0, 0, power_profiles);
    if(!mutable_power_profiles) goto exit;
    
    // Prune unsupported power supplies and unsupported
    // settings
    p_count = CFArrayGetCount(mutable_power_profiles);
    for(i=0; i<p_count; i++)
    {
        tmp = CFArrayGetValueAtIndex(power_profiles, i);
        if(!tmp) continue;
        mutable_profile = CFDictionaryCreateMutableCopy(
            kCFAllocatorDefault, 
            0, 
            tmp);
        if(!mutable_profile) continue;
        IOPMRemoveIrrelevantProperties(mutable_profile);
        CFArraySetValueAtIndex(mutable_power_profiles, i, mutable_profile);
        CFRelease(mutable_profile);
    }
exit:
    if(power_profiles) CFRelease(power_profiles);    
    return mutable_power_profiles;
}

static int _isActiveProfileDictValid(CFDictionaryRef p)
{
    CFNumberRef     val;
    int             j;
    
    if(!p) return 0;
    
    // AC value required
    val = CFDictionaryGetValue(p, CFSTR(kIOPMACPowerKey));
    if(!val) return 0;
    CFNumberGetValue(val, kCFNumberIntType, &j);
    if(j<-1 || j>= kIOPMNumPowerProfiles) return 0;

    // Battery value optional
    val = CFDictionaryGetValue(p, CFSTR(kIOPMBatteryPowerKey));
    if(val) {
        CFNumberGetValue(val, kCFNumberIntType, &j);
        if(j<-1 || j>= kIOPMNumPowerProfiles) return 0;
    }
    
    // UPS value optional
    val = CFDictionaryGetValue(p, CFSTR(kIOPMUPSPowerKey));
    if(val) {
        CFNumberGetValue(val, kCFNumberIntType, &j);
        if(j<-1 || j>= kIOPMNumPowerProfiles) return 0;
    }
    
    return 1;    
}

static void _purgeUnsupportedPowerSources(CFMutableDictionaryRef p)
{
    CFStringRef                     *ps_names = NULL;
    CFTypeRef                       ps_snap = NULL;
    int                             count;
    int                             i;

    ps_snap = IOPSCopyPowerSourcesInfo();
    if(!ps_snap) return;
    count = CFDictionaryGetCount(p);
    ps_names = (CFStringRef *)malloc(count*sizeof(CFStringRef));
    if(!ps_names) goto exit;
    CFDictionaryGetKeysAndValues(p, (CFTypeRef *)ps_names, NULL);
    for(i=0; i<count; i++)
    {
        if(kCFBooleanTrue != IOPSPowerSourceSupported(ps_snap, ps_names[i])) {
            CFDictionaryRemoveValue(p, ps_names[i]);
        }    
    }
exit:
    if(ps_snap) CFRelease(ps_snap);
    if(ps_names) free(ps_names);
}

// How we determine what ActivePowerProfiles to return:
// (1) If the user has specified any profiles in the
//     PM prefs file, we'll return those.
// (2) If the user hasn't explicitly specified any
//     profiles, we'll look to their Custom settings:
// (3) If, in the past, a user has specified any PM settings at
//     all (say from before an upgrade install), we'll return
//     (-1, -1, -1) and respect those settings as custom settings.
// (4) If there are no specified profiles and no previous specified
//     custom settings, then we'll return (2, 1, 1) as specified
//     in PowerManagement.bundle's com.apple.SystemPowerProfileDefaults.plist
//
// Also, note that in the case of (1), we'll re-populate any settings not
// specified for a particular power source (UPS's are easy to plug/unplug)
// with the defaults obtained as in (4).
//
// For all steps above, we'll strip any irrelevant settings before
// returning the dictionary (i.e. battery or UPS settings will not be 
// returned where unsupported)

CFDictionaryRef     IOPMCopyActivePowerProfiles(void)
{
    SCPreferencesRef            energyPrefs = NULL;
    CFDictionaryRef             tmp = NULL;
    CFDictionaryRef             tmp_custom = NULL;
    CFDictionaryRef             defaultProfiles = NULL;
    CFMutableDictionaryRef      activeProfiles = NULL;
    CFStringRef                 *profileKeys = NULL;
    CFNumberRef                 *profileValues = NULL;
    bool                        activeProfilesSpecified = false;
    bool                        customSettingsSpecified = false;
    int                         profileCount;
    int                         i;
    
    energyPrefs = SCPreferencesCreate( kCFAllocatorDefault, kIOPMAppName, kIOPMPrefsPath );
    if(!energyPrefs) return NULL;

    tmp = SCPreferencesGetValue(energyPrefs, CFSTR("ActivePowerProfiles"));
    if(tmp && _isActiveProfileDictValid(tmp)) {
        activeProfiles = CFDictionaryCreateMutableCopy(0, 0, tmp);
        activeProfilesSpecified = true;
    } else {
        activeProfiles = CFDictionaryCreateMutable(0, 3, 
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    }
    if(!activeProfiles) goto exit;
    
    // Determine if there are any user-specified settings for Energy Saver
    // not a profile selection, but a custom dictionary
    tmp_custom = IOPMCopyPMPreferences();
    if(isA_CFDictionary(tmp_custom))
    {
        if(kCFBooleanTrue != CFDictionaryGetValue(tmp_custom, CFSTR("Defaults")))
        {
            customSettingsSpecified = true;
        }
        CFRelease(tmp_custom);
    }
    
    if(!activeProfilesSpecified && customSettingsSpecified)
    {
        defaultProfiles = _createAllCustomProfileSelections();        
    } else {
        defaultProfiles = _createDefaultProfileSelections();
    }
    
    // Merge default profiles into active profiles
    // If the user has any specified Profiles, we'll merge in the defaults
    // for any power sources that aren't specified. If there weren't any specified
    // profile choices, we'll just merge in (-1, -1, -1) for all custom or
    // (2, 1, 1) to the empty dictionary, as described in comments above.
    if(isA_CFDictionary(defaultProfiles))
    {
        profileCount = CFDictionaryGetCount(defaultProfiles);
        profileKeys = malloc(sizeof(CFStringRef)*profileCount);
        profileValues = malloc(sizeof(CFNumberRef)*profileCount);
        if(!profileKeys || !profileValues) goto exit;
        CFDictionaryGetKeysAndValues(defaultProfiles, 
                (const void **)profileKeys, (const void **)profileValues);
        for(i=0; i<profileCount; i++)
        {
            if( isA_CFString(profileKeys[i]) &&
                isA_CFNumber(profileValues[i]) )
            {
                // use the softer AddValue that won't replace any already present
                // settings in the existing chosen profiles dictionary.
                CFDictionaryAddValue(activeProfiles, profileKeys[i], profileValues[i]);            
            }
        }
        free(profileKeys);
        free(profileValues);
    }
    
    // And remove all the unsupported profiles that we just added
    _purgeUnsupportedPowerSources(activeProfiles);
  
exit:
    if(energyPrefs) CFRelease(energyPrefs);
    if(defaultProfiles) CFRelease(defaultProfiles);
    return activeProfiles;
}

IOReturn IOPMSetActivePowerProfiles(CFDictionaryRef which_profile)
{
    CFDataRef           profiles_data;
    vm_address_t        profiles_buffer;
    IOByteCount         buffer_len;
    kern_return_t       kern_result;
    IOReturn            return_val = kIOReturnError;
    mach_port_t         server_port = MACH_PORT_NULL;

    if(!_isActiveProfileDictValid(which_profile)) {
        return kIOReturnBadArgument;
    }
    
    // open reference to PM configd
    kern_result = bootstrap_look_up(bootstrap_port, 
            kIOPMServerBootstrapName, &server_port);    
    if(KERN_SUCCESS != kern_result) {
        return kIOReturnError;
    }

    profiles_data = IOCFSerialize(which_profile, 0);
    profiles_buffer = (vm_address_t) CFDataGetBytePtr(profiles_data);
    buffer_len = CFDataGetLength(profiles_data);

    // toss dictionary over the wall to conigd via mig-generated interface
    // configd will perform a permissions check. If the caller is root,
    // admin, or console, configd will write the prefs file from its
    // root context.
    kern_result = io_pm_set_active_profile(server_port, 
                            profiles_buffer, buffer_len, 
                            &return_val);

    mach_port_destroy(mach_task_self(), server_port);
    CFRelease(profiles_data);

    if(KERN_SUCCESS == kern_result) {
        return return_val;
    } else {
        return kIOReturnInternalError;
    }
}


/**************************************************
*
* Support structures and functions for IOPMPrefsNotificationCreateRunLoopSource
*
**************************************************/
typedef struct {
    IOPMPrefsCallbackType       callback;
    void                        *context;
} user_callback_context;

/* SCDynamicStoreCallback */
static void ioCallout(SCDynamicStoreRef store, CFArrayRef keys, void *ctxt) {
    user_callback_context   *c; 
    IOPowerSourceCallbackType cb;

    c = (user_callback_context *)CFDataGetBytePtr((CFDataRef)ctxt);
    if(!c) return;
    cb = c->callback;
    if(!cb) return;
    
    // Execute callback
    (*cb)(c->context);
}

/***
 Returns a CFRunLoopSourceRef that notifies the caller when power source
 information changes.
 Arguments:
    IOPowerSourceCallbackType callback - A function to be called whenever ES prefs file on disk changes
    void *context - Any user-defined pointer, passed to the IOPowerSource callback.
 Returns NULL if there were any problems.
 Caller must CFRelease() the returned value.
***/
CFRunLoopSourceRef IOPMPrefsNotificationCreateRunLoopSource(IOPMPrefsCallbackType callback, void *context) {
    SCDynamicStoreRef           store = NULL;
    CFStringRef                 EnergyPrefsKey = NULL;
    CFRunLoopSourceRef          SCDrls = NULL;
    user_callback_context       *ioContext = NULL;
    SCDynamicStoreContext       scContext = {0, NULL, CFRetain, CFRelease, NULL};

    if(!callback) return NULL;

    scContext.info = CFDataCreateMutable(NULL, sizeof(user_callback_context));
    CFDataSetLength(scContext.info, sizeof(user_callback_context));
    ioContext = (user_callback_context *)CFDataGetBytePtr(scContext.info); 
    ioContext->context = context;
    ioContext->callback = callback;
        
    // Open connection to SCDynamicStore. User's callback as context.
    store = SCDynamicStoreCreate(kCFAllocatorDefault, 
                CFSTR("IOKit Preferences Copy"), ioCallout, (void *)&scContext);
    if(!store) return NULL;
     
    // Setup notification for changes in Energy Saver prefences
    EnergyPrefsKey = SCDynamicStoreKeyCreatePreferences(
                    NULL, 
                    kIOPMPrefsPath, 
                    kSCPreferencesKeyApply);
    if(EnergyPrefsKey) {
        SCDynamicStoreAddWatchedKey(store, EnergyPrefsKey, FALSE);
        CFRelease(EnergyPrefsKey);
    }

    // Obtain the CFRunLoopSourceRef from this SCDynamicStoreRef session
    SCDrls = SCDynamicStoreCreateRunLoopSource(kCFAllocatorDefault, store, 0);
    CFRelease(store);

    return SCDrls;
}

