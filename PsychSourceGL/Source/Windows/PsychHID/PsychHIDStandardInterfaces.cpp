/*
 PsychToolbox3/Source/Windows/PsychHID/PsychHIDStandardInterfaces.c
 
 PROJECTS: PsychHID only.
 
 PLATFORMS:  Windows.
 
 AUTHORS:
 mario.kleiner@tuebingen.mpg.de    mk
 
 HISTORY:
 9.08.2011     mk     Created.
 
 TO DO:
 * Classic KbCheck for multiple keyboards.
 * Gamepad support.
 * Code cleanup.
 * Maybe use 32-Bit timestamps delivered from the os instead of rolling our own?

*/

// Define direct input version explicitely to avoid compiler warnings.
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include "PsychHIDStandardInterfaces.h"

#ifdef __cplusplus
extern "C" {
#endif

static int ndevices = 0;

// Pointer to DirectInput-8 interface:
LPDIRECTINPUT8 dinput = NULL;

// GUID identifiers of all enumerated keyboards:
typedef struct dinfo {
    GUID guidInstance;
    GUID guidProduct;
    psych_uint16 usagePage;
    psych_uint16 usageValue;
    psych_uint32 dwDevType;
    TCHAR tszInstanceName[MAX_PATH];
    TCHAR tszProductName[MAX_PATH];
} dinfo;

static dinfo info[PSYCH_HID_MAX_DEVICES];
LPDIRECTINPUTDEVICE8 x_dev[PSYCH_HID_MAX_DEVICES];

static	double* psychHIDKbQueueFirstPress[PSYCH_HID_MAX_DEVICES];
static	double* psychHIDKbQueueFirstRelease[PSYCH_HID_MAX_DEVICES];
static	double* psychHIDKbQueueLastPress[PSYCH_HID_MAX_DEVICES];
static	double* psychHIDKbQueueLastRelease[PSYCH_HID_MAX_DEVICES];
static  int*    psychHIDKbQueueScanKeys[PSYCH_HID_MAX_DEVICES];
static  psych_bool psychHIDKbQueueActive[PSYCH_HID_MAX_DEVICES];
static  psych_mutex KbQueueMutex;
static  psych_condition KbQueueCondition;
static	psych_bool  KbQueueThreadTerminate;
static  psych_thread KbQueueThread;
static  HANDLE hEvent;

// Enumeration callback from DirectInput-8 for keyboard enumeration.
// Called once for each detected keyboard. lpddi points to the following
// info structure:
//
//typedef struct DIDEVICEINSTANCE {
//    DWORD dwSize;
//    GUID guidInstance;
//    GUID guidProduct;
//    DWORD dwDevType;
//    TCHAR tszInstanceName[MAX_PATH];
//    TCHAR tszProductName[MAX_PATH];
//    GUID guidFFDriver;
//    WORD wUsagePage;
//    WORD wUsage;
//} DIDEVICEINSTANCE, *LPDIDEVICEINSTANCE;
BOOL keyboardEnumCallback(LPCDIDEVICEINSTANCE lpddi, LPVOID pvRef)
{
    // Useless assignment to make compiler happy:
    pvRef = NULL;
    
    // Copy relevant info into our own info array:
    info[ndevices].guidInstance = lpddi->guidInstance;
    info[ndevices].guidProduct = lpddi->guidProduct;
    info[ndevices].usagePage = lpddi->wUsagePage;
    info[ndevices].usageValue = lpddi->wUsage;
    info[ndevices].dwDevType = lpddi->dwDevType;
    memcpy(&info[ndevices].tszInstanceName[0], &(lpddi->tszInstanceName[0]), MAX_PATH * sizeof(TCHAR));
    memcpy(&info[ndevices].tszProductName[0], &(lpddi->tszProductName[0]), MAX_PATH * sizeof(TCHAR));
    
    ndevices++;
    
    // Done. Continue with enumeration, unless the capacity of our internal
    // array is exhausted:
    if (ndevices == PSYCH_HID_MAX_DEVICES) printf("PsychHID-WARNING: Number of detected keyboard devices %i now equal to our maximum capacity. May miss some keyboard devices!\n", ndevices);    
    
    return((ndevices < PSYCH_HID_MAX_DEVICES) ? DIENUM_CONTINUE : DIENUM_STOP);
}

void PsychHIDInitializeHIDStandardInterfaces(void)
{
	int i;
    HRESULT rc;
    
    dinput = NULL;
    ndevices = 0;
    
	// Init x_dev array:
	for (i = 0; i < PSYCH_HID_MAX_DEVICES; i++) x_dev[i] = NULL;
    
	// Init keyboard queue arrays:
	memset(&psychHIDKbQueueFirstPress[0], 0, sizeof(psychHIDKbQueueFirstPress));
	memset(&psychHIDKbQueueFirstRelease[0], 0, sizeof(psychHIDKbQueueFirstRelease));
	memset(&psychHIDKbQueueLastPress[0], 0, sizeof(psychHIDKbQueueLastPress));
	memset(&psychHIDKbQueueLastRelease[0], 0, sizeof(psychHIDKbQueueLastRelease));
	memset(&psychHIDKbQueueActive[0], 0, sizeof(psychHIDKbQueueActive));
	memset(&psychHIDKbQueueScanKeys[0], 0, sizeof(psychHIDKbQueueScanKeys));
    
    // Open a DirectInput-8 interface:
    rc = DirectInput8Create(GetModuleHandle(NULL), DIRECTINPUT_VERSION, IID_IDirectInput8, (LPVOID*)&dinput, NULL);
    
	if (DI_OK != rc) {
        printf("PsychHID-ERROR: Error return from DirectInput8Create: %i\n", (int) rc);
		PsychErrorExitMsg(PsychError_system, "PsychHID: FATAL ERROR: Couldn't create interface to Microsoft DirectInput-8! Game over!");
	}
    
	// Enumerate all DirectInput keyboard(-like) devices:
    rc = dinput->EnumDevices(DI8DEVCLASS_KEYBOARD, (LPDIENUMDEVICESCALLBACK) keyboardEnumCallback, NULL, DIEDFL_ATTACHEDONLY | DIEDFL_INCLUDEALIASES | DIEDFL_INCLUDEHIDDEN);
    if (DI_OK != rc) {
        printf("PsychHID-ERROR: Error return from DirectInput8 EnumDevices(): %i! Game over!\n", (int) rc);
        goto out;
	}
    
	// Create keyboard queue mutex for later use:
	KbQueueThreadTerminate = FALSE;
	PsychInitMutex(&KbQueueMutex);
	PsychInitCondition(&KbQueueCondition, NULL);
    
    // Create event object for signalling device state changes:
    hEvent = CreateEvent(   NULL,	// default security attributes
                            FALSE,	// auto-reset event: This would need to be set TRUE for PsychBroadcastCondition() to work on Windows!
                            FALSE,	// initial state is nonsignaled
                            NULL	// no object name
                            ); 
    
    // Ready.
	return;
    
out:
        ndevices = 0;
    
	// Close our dedicated x-display connection and we are done:
    if (dinput) dinput->Release();
    dinput = NULL;
    
	PsychErrorExitMsg(PsychError_system, "PsychHID: FATAL ERROR: X Input extension version 2.0 or later not available! Game over!");	
}

static LPDIRECTINPUTDEVICE8 GetXDevice(int deviceIndex)
{
	if (deviceIndex < 0 || deviceIndex >= PSYCH_HID_MAX_DEVICES || deviceIndex >= ndevices) PsychErrorExitMsg(PsychError_user, "Invalid deviceIndex specified. No such device!");
	if (x_dev[deviceIndex] == NULL) {
        if (DI_OK != dinput->CreateDevice(info[deviceIndex].guidInstance, &(x_dev[deviceIndex]), NULL)) {
            PsychErrorExitMsg(PsychError_user, "Could not open connection to device. CreateDevice() failed!");
        }
    }
	return(x_dev[deviceIndex]);
}

void PsychHIDShutdownHIDStandardInterfaces(void)
{
	int i;
    
	// Release all keyboard queues:
	for (i = 0; i < PSYCH_HID_MAX_DEVICES; i++) {
		if (psychHIDKbQueueFirstPress[i]) {
			PsychHIDOSKbQueueRelease(i);
		}
	}
    
	// Close all devices registered in x_dev array:
	for (i = 0; i < PSYCH_HID_MAX_DEVICES; i++) {
		if (x_dev[i]) x_dev[i]->Release();
		x_dev[i] = NULL;
	}
    
	// Release keyboard queue mutex:
	PsychDestroyMutex(&KbQueueMutex);
	PsychDestroyCondition(&KbQueueCondition);
	KbQueueThreadTerminate = FALSE;
    
    if (!CloseHandle(hEvent)) {
		printf("PsychHID-WARNING: Closing keyboard event handle failed!\n");
    }
    
    ndevices = 0;
    
	// Close our dedicated x-display connection and we are done:
    if (dinput) dinput->Release();
    dinput = NULL;
    
	return;
}

PsychError PsychHIDEnumerateHIDInputDevices(int deviceClass)
{
    const char *deviceFieldNames[]={"usagePageValue", "usageValue", "usageName", "index", "transport", "vendorID", "productID", "version", 
        "manufacturer", "product", "serialNumber", "locationID", "interfaceID", "totalElements", "features", "inputs", 
        "outputs", "collections", "axes", "buttons", "hats", "sliders", "dials", "wheels"};
    int numDeviceStructElements, numDeviceStructFieldNames=24, deviceIndex;
    PsychGenericScriptType	*deviceStruct;
    dinfo *dev;
    int i;
    int numKeys, numAxis;
    char *type = "";
    
    // Preparse: Count matching devices for deviceClass
    numDeviceStructElements = ndevices;
    /*
     for(i = 0; i < ndevices; i++) {
         dev = &info[i];
         if ((int) (dev->use) == deviceClass) numDeviceStructElements++;
     }
     */
    
    // Alloc struct array of sufficient size:
    PsychAllocOutStructArray(1, kPsychArgOptional, numDeviceStructElements, numDeviceStructFieldNames, deviceFieldNames, &deviceStruct);
    deviceIndex = 0;
    
    // Return info:
    for(i = 0; i < ndevices; i++) {
        // Check i'th device:
        dev = &info[i];
        
        // Skip if non matching class:
        // if ((int) (dev->use) != deviceClass) continue;
        
        switch(dev->dwDevType & 0xff) {
            //case DI8DEVTYPE_MOUSE: type = "master pointer"; break;
            //case DI8DEVTYPE_KEYBOARD: type = "master keyboard"; break;
            case DI8DEVTYPE_MOUSE:
				type = "slave pointer";
				if (dev->usagePage == 0) dev->usagePage = 1;
				if (dev->usageValue == 0) dev->usageValue = 2;
			break;

			case DI8DEVTYPE_KEYBOARD:
				type = "slave keyboard";
				if (dev->usagePage == 0) dev->usagePage = 1;
				if (dev->usageValue == 0) dev->usageValue = 6;
			break;
        }
        
        PsychSetStructArrayDoubleElement("usagePageValue",	deviceIndex, 	(double) dev->usagePage, deviceStruct);        
        PsychSetStructArrayDoubleElement("usageValue",	deviceIndex,        (double) dev->usageValue, deviceStruct);
        
        PsychSetStructArrayStringElement("usageName",		deviceIndex, 	type, deviceStruct);
        PsychSetStructArrayDoubleElement("index",           deviceIndex, 	(double) i, deviceStruct);
        PsychSetStructArrayStringElement("transport",		deviceIndex, 	dev->tszInstanceName, deviceStruct);
        PsychSetStructArrayStringElement("product",         deviceIndex, 	dev->tszProductName, deviceStruct);
        PsychSetStructArrayDoubleElement("locationID",		deviceIndex, 	(double) -1, deviceStruct);
        PsychSetStructArrayDoubleElement("interfaceID",		deviceIndex, 	(double) -1, deviceStruct);
        PsychSetStructArrayDoubleElement("productID",		deviceIndex, 	(double) -1, deviceStruct);
        
        //PsychSetStructArrayDoubleElement("vendorID",		deviceIndex, 	(double)currentDevice->vendorID, 	deviceStruct);
        //PsychSetStructArrayDoubleElement("version",		deviceIndex, 	(double)currentDevice->version, 	deviceStruct);
        //PsychSetStructArrayStringElement("manufacturer",	deviceIndex, 	currentDevice->manufacturer, 		deviceStruct);
        //PsychSetStructArrayStringElement("serialNumber",	deviceIndex, 	currentDevice->serial, 			deviceStruct);
        
        numKeys = numAxis = 0;
        
        PsychSetStructArrayDoubleElement("totalElements",	deviceIndex, 	(double) numKeys + numAxis, deviceStruct);
        PsychSetStructArrayDoubleElement("features",		deviceIndex, 	(double) 0, deviceStruct);
        PsychSetStructArrayDoubleElement("inputs",          deviceIndex, 	(double) numKeys + numAxis, deviceStruct);
        PsychSetStructArrayDoubleElement("outputs",         deviceIndex, 	(double) 0, deviceStruct);
        PsychSetStructArrayDoubleElement("collections",     deviceIndex, 	(double) 0, deviceStruct);
        PsychSetStructArrayDoubleElement("axes",		deviceIndex, 	(double) numAxis, deviceStruct);
        PsychSetStructArrayDoubleElement("buttons",		deviceIndex, 	(double) numKeys, deviceStruct);
        PsychSetStructArrayDoubleElement("hats",		deviceIndex, 	(double) 0, deviceStruct);
        PsychSetStructArrayDoubleElement("sliders",		deviceIndex, 	(double) 0, deviceStruct);
        PsychSetStructArrayDoubleElement("dials",		deviceIndex, 	(double) 0, deviceStruct);
        PsychSetStructArrayDoubleElement("wheels",		deviceIndex, 	(double) 0, deviceStruct);
        deviceIndex++;
    }
    
    return(PsychError_none);
}

PsychError PsychHIDOSKbCheck(int deviceIndex, double* scanList)
{
    
    // MK FIXME - TODO:Could implement via: kb->GetDeviceState
    
    /*
     PsychNativeBooleanType* buttonStates;
     unsigned char keys_return[32];
     int keysdown;
     double timestamp;
     int i, j;
     
     // Map "default" deviceIndex to legacy "Core protocol" method of querying keyboard
     // state. This will give us whatever X has setup as default keyboard:
     if (deviceIndex == INT_MAX) {
         // Request current keyboard state of default keyboard from X-Server:
         XQueryKeymap(dpy, keys_return);
     } else if (deviceIndex < 0 || deviceIndex >= ndevices) {
         PsychErrorExitMsg(PsychError_user, "Invalid keyboard deviceIndex specified. No such device!");
     } else if (info[deviceIndex].use == XIMasterKeyboard) {
         // Master keyboard:
         
         // Query current client pointer assignment, then switch it to
         // associated master pointer for the master keyboard we want
         // to query. This way, all future queries will query our requested
         // master keyboard:
         j = -1;
         if (!XIGetClientPointer(dpy, None, &j) || (j != info[deviceIndex].attachment)) XISetClientPointer(dpy, None, info[deviceIndex].attachment);
         
         // Request current keyboard state from X-Server:
         XQueryKeymap(dpy, keys_return);
         
         // Reset master pointer/keyboard assignment to pre-query state:
         if ((j > 0) && (j != info[deviceIndex].attachment)) XISetClientPointer(dpy, None, j);
     } else {
         // Non-Default deviceIndex: Want to query specific slave keyboard.
         // Validate it maps to a slave keyboard device, as we can't handle
         // master keyboard devices this way and don't want to touch anything
         // but a keyboard'ish device:
         if (info[deviceIndex].use != XISlaveKeyboard) {
             PsychErrorExitMsg(PsychError_user, "Invalid keyboard deviceIndex specified. Not a slave keyboard device!");
         }
         
         // Open connection to slave keyboard device:
         XDevice* mydev = GetXDevice(deviceIndex);
         
         // Query its current state:
         XDeviceState* state = XQueryDeviceState(dpy, mydev);
         XInputClass* data = state->data;
         
         // printf("Dummy = %i , NClasses = %i\n", dummy1, state->num_classes);
         
         // Find state structure with key status info:
         for (i = 0; i < state->num_classes; i++) {
             // printf("Class %i: Type %i - %i\n", i, (int) data->class, (int) data->length);
             if (data->class == KeyClass) {
                 // printf("NumKeys %i\n", ((XKeyState*) data)->num_keys);
                 
                 // Copy 32 Byte keystate vector into key_return. Each bit encodes for one key:
                 memcpy(&keys_return[0], &(((XKeyState*) data)->keys[0]), sizeof(keys_return));
             }
             
             data = (XInputClass*) (((void*) data) + ((size_t) data->length));
         }
         
         XFreeDeviceState(state);
     }
     
     // Done with query. We have keyboard state in keys_return[] now.
     
     // Request current time of query:
     PsychGetAdjustedPrecisionTimerSeconds(&timestamp);
     
     // Reset overall key state to "none pressed":
     keysdown = 0;
     
     // Any key down?
     for (i = 0; i < 32; i++) keysdown+=(unsigned int) keys_return[i];
     
     // Copy out overall keystate:
     PsychCopyOutDoubleArg(1, kPsychArgOptional, (keysdown > 0) ? 1 : 0);
     
     // Copy out timestamp:
     PsychCopyOutDoubleArg(2, kPsychArgOptional, timestamp);
     
     // Copy keyboard state:
     PsychAllocOutBooleanMatArg(3, kPsychArgOptional, 1, 256, 1, &buttonStates);
     
     // Map 32 times 8 bitvector to 256 element return vector:
     for(i = 0; i < 32; i++) {
         for(j = 0; j < 8; j++) {
             // This key down?
             buttonStates[i*8 + j] = (PsychNativeBooleanType) (keys_return[i] & (1<<j)) ? 1 : 0;
             // Apply scanList mask, if any provided:
             if (scanList && (scanList[i*8 + j] <= 0)) buttonStates[i*8 + j] = (PsychNativeBooleanType) 0;
         }
     }
     */
	return(PsychError_none);
}

PsychError PsychHIDOSGamePadAxisQuery(int deviceIndex, int axisId, double* min, double* max, double* val, char* axisLabel)
{
	return(PsychError_none);
}

// This is the event dequeue & process function which updates
// Keyboard queue state. It can be called with 'blockingSinglepass'
// set to TRUE to process exactly one event, if called from the
// background keyboard queue processing thread. Alternatively it
// can be called synchronously from KbQueueCheck with a setting of FALSE
// to iterate over all available events and process them instantaneously:
void KbQueueProcessEvents(psych_bool blockingSinglepass)
{
    LPDIRECTINPUTDEVICE8 kb;
    DIDEVICEOBJECTDATA event;
    HRESULT rc;
    DWORD dwItems; 
	double tnow;
	unsigned int i, keycode, keystate;
    
	while (1) {
        
		// Single pass or multi-pass?
		if (blockingSinglepass) {
			// Wait until at least one event available and dequeue it:
            // We use a timeout of 100 msecs.
            WaitForSingleObject(hEvent, 100);
		} else {
			// Check if event available, dequeue it, if so. Abort
			// processing if no new event available, aka queue empty:
			// TODO if (!XCheckTypedEvent(thread_dpy, GenericEvent, &KbQueue_xevent)) break;
		}
        
		// Take timestamp:
		PsychGetAdjustedPrecisionTimerSeconds(&tnow);
        
        // Need the lock from here on:
        PsychLockMutex(&KbQueueMutex);
        
        // Do a sweep over all keyboard devices whose queues are active:
        for (i = 0; i < (unsigned int) ndevices; i++) {
            // Skip this one if inactive:
            if (!psychHIDKbQueueActive[i]) continue;
            
            // Check this device:
            kb = GetXDevice(i);
            
            // Fetch one item from the buffer:
            // event.dwTimeStamp = Timestamp in msecs of timeGetTime() timebase.
            // event.dwSequence = Sequence number.
            
            // Fetch from this device, item-by-item, until nothing more to fetch:
            while (TRUE) {
                // Try one fetch from this device:
                dwItems = 1;
                rc = kb->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), &event, &dwItems, 0);
                
                // If failed or nothing more to fetch, break out of fetch loop:
                if (!SUCCEEDED(rc) || (0 == dwItems)) break;
                
                // Map to key code and key state:
                keycode = event.dwOfs & 0xff;
                keystate = event.dwData & 0x80;
                
				// Map scancode 'keycode' to virtual key code 'keycode':
				#ifndef MAPVK_VSC_TO_VK_EX
				#define MAPVK_VSC_TO_VK_EX 3
				#endif

				// Must also shift by one count to account for difference 1-based vs. zero-based indexing:
				keycode = MapVirtualKeyEx(keycode, MAPVK_VSC_TO_VK_EX, GetKeyboardLayout(0));
				if (keycode > 0) {
					// Translated keycode: Adapt for 1 offset:
					keycode--;
				} else {
					// Untranslated keycode: Refetch raw code:
					keycode = event.dwOfs & 0xff;
					// TODO: Manually map the most important ones, e.g., Cursor keys!
				}

                // This keyboard queue interested in this keycode?
                if (psychHIDKbQueueScanKeys[i][keycode] != 0) {
                    // Yes: The queue wants to receive info about this key event.
                    
                    // Press or release?
                    if (keystate) {
                        // Enqueue key press. Always in the "last press" array, because any
                        // press at this time is the best candidate for the last press.
                        // Only enqeue in "first press" if there wasn't any registered before,
                        // ie., the slot is so far empty:
                        if (psychHIDKbQueueFirstPress[i][keycode] == 0) psychHIDKbQueueFirstPress[i][keycode] = tnow;
                        psychHIDKbQueueLastPress[i][keycode] = tnow;
                    } else {
                        // Enqueue key release. See logic above:
                        if (psychHIDKbQueueFirstRelease[i][keycode] == 0) psychHIDKbQueueFirstRelease[i][keycode] = tnow;
                        psychHIDKbQueueLastRelease[i][keycode] = tnow;
                    }
                    
                    // Tell waiting userspace something interesting has changed:
                    PsychSignalCondition(&KbQueueCondition);
                }
                // Next fetch iteration for this device...
            }
            // Check next device...
        }
        
        // Done with shared data access:
        PsychUnlockMutex(&KbQueueMutex);
        
		// Done if we were only supposed to handle one sweep, which we did:
		if (blockingSinglepass) break;
	}
    
	return;
}

// Async processing thread for keyboard events:
void* KbQueueWorkerThreadMain(void* dummy)
{
	int rc;
    
	// Try to raise our priority: We ask to switch ourselves (NULL) to priority class 1 aka
	// realtime scheduling, with a tweakPriority of +1, ie., raise the relative
	// priority level by +1 wrt. to the current level:
	if ((rc = PsychSetThreadPriority(NULL, 1, 1)) > 0) {
		printf("PsychHID: KbQueueStart: Failed to switch to realtime priority [%s].\n", strerror(rc));
	}
    
	while (1) {
		PsychLockMutex(&KbQueueMutex);
        
		// Check if we should terminate:
		if (KbQueueThreadTerminate) break;
        
		PsychUnlockMutex(&KbQueueMutex);
        
		// Perform event processing until no more events are pending:
		KbQueueProcessEvents(TRUE);
	}
    
	// Done. Unlock the mutex:
	PsychUnlockMutex(&KbQueueMutex);
    
	// printf("DEBUG: THREAD TERMINATING...\n"); fflush(NULL);
    
	// Return and terminate:
	return(NULL);
}

static int PsychHIDGetDefaultKbQueueDevice(void)
{
    // Return first enumerated keyboard (index == 0) if any available:
    if (ndevices > 0) return(0);
    
    // Nothing found? If so, abort:
    PsychErrorExitMsg(PsychError_user, "Could not find any useable keyboard device!");

	// Utterly bogus return to make crappy Microsoft compiler shut up:
	return(0);
}

PsychError PsychHIDOSKbQueueCreate(int deviceIndex, int numScankeys, int* scanKeys)
{
	dinfo* dev = NULL;
    
	// Valid number of keys?
	if (scanKeys && (numScankeys != 256)) {
		PsychErrorExitMsg(PsychError_user, "Second argument to KbQueueCreate must be a vector with 256 elements.");
	}
    
	if (deviceIndex < 0) {
		deviceIndex = PsychHIDGetDefaultKbQueueDevice();
		// Ok, deviceIndex now contains our default keyboard to use - The first suitable keyboard.
	} else if (deviceIndex >= ndevices) {
		// Out of range index:
		PsychErrorExitMsg(PsychError_user, "Invalid keyboard 'deviceIndex' specified. No such device!");
	} 
    
	// Do we finally have a valid keyboard?
	dev = &info[deviceIndex];
	if ((dev->dwDevType & 0xff) != DI8DEVTYPE_KEYBOARD) {
		PsychErrorExitMsg(PsychError_user, "Invalid keyboard 'deviceIndex' specified. Not a keyboard device!");
	}
    
	// Keyboard queue for this deviceIndex already created?
	if (psychHIDKbQueueFirstPress[deviceIndex]) {
		// Yep. Release it, so we can start from scratch:
		PsychHIDOSKbQueueRelease(deviceIndex);
	}
    
	// Allocate and zero-init memory for tracking key presses and key releases:
	psychHIDKbQueueFirstPress[deviceIndex]   = (double*) calloc(256, sizeof(double));
	psychHIDKbQueueFirstRelease[deviceIndex] = (double*) calloc(256, sizeof(double));
	psychHIDKbQueueLastPress[deviceIndex]    = (double*) calloc(256, sizeof(double));
	psychHIDKbQueueLastRelease[deviceIndex]  = (double*) calloc(256, sizeof(double));
	psychHIDKbQueueScanKeys[deviceIndex]     = (int*) calloc(256, sizeof(int));
    
	// Assign scanKeys vector, if any:
	if (scanKeys) {
		// Copy it:
		memcpy(psychHIDKbQueueScanKeys[deviceIndex], scanKeys, 256 * sizeof(int));
	} else {
		// None provided. Enable all keys by default:
		memset(psychHIDKbQueueScanKeys[deviceIndex], 1, 256 * sizeof(int));        
	}
    
	// Ready to use this keybord queue.
	return(PsychError_none);
}

void PsychHIDOSKbQueueRelease(int deviceIndex)
{
	if (deviceIndex < 0) {
		deviceIndex = PsychHIDGetDefaultKbQueueDevice();
		// Ok, deviceIndex now contains our default keyboard to use - The first suitable keyboard.
	}
    
	if ((deviceIndex < 0) || (deviceIndex >= ndevices)) {
		// Out of range index:
		PsychErrorExitMsg(PsychError_user, "Invalid keyboard 'deviceIndex' specified. No such device!");
	}
    
	// Keyboard queue for this deviceIndex already exists?
	if (NULL == psychHIDKbQueueFirstPress[deviceIndex]) {
		// No. Nothing to do then.
		return;
	}
    
	// Ok, we have a keyboard queue. Stop any operation on it first:
	PsychHIDOSKbQueueStop(deviceIndex);
    
	// Release its data structures:
	free(psychHIDKbQueueFirstPress[deviceIndex]); psychHIDKbQueueFirstPress[deviceIndex] = NULL;
	free(psychHIDKbQueueFirstRelease[deviceIndex]); psychHIDKbQueueFirstRelease[deviceIndex] = NULL;
	free(psychHIDKbQueueLastPress[deviceIndex]); psychHIDKbQueueLastPress[deviceIndex] = NULL;
	free(psychHIDKbQueueLastRelease[deviceIndex]); psychHIDKbQueueLastRelease[deviceIndex] = NULL;
	free(psychHIDKbQueueScanKeys[deviceIndex]); psychHIDKbQueueScanKeys[deviceIndex] = NULL;
    
	// Done.
	return;
}

void PsychHIDOSKbQueueStop(int deviceIndex)
{
    LPDIRECTINPUTDEVICE8 kb;
	psych_bool queueActive;
	int i;
    
	if (deviceIndex < 0) {
		deviceIndex = PsychHIDGetDefaultKbQueueDevice();
		// Ok, deviceIndex now contains our default keyboard to use - The first suitable keyboard.
	}
    
	if ((deviceIndex < 0) || (deviceIndex >= ndevices)) {
		// Out of range index:
		PsychErrorExitMsg(PsychError_user, "Invalid keyboard 'deviceIndex' specified. No such device!");
	}
    
	// Keyboard queue for this deviceIndex already exists?
	if (NULL == psychHIDKbQueueFirstPress[deviceIndex]) {
		// No. Nothing to do then.
		return;
	}
    
	// Keyboard queue already stopped?
	if (!psychHIDKbQueueActive[deviceIndex]) return;
    
    // Get device:
    kb = GetXDevice(deviceIndex);    
    
	// Queue is active. Stop it:
	PsychLockMutex(&KbQueueMutex);

	// Release the device:
    if (DI_OK != kb->Unacquire()) {
        PsychUnlockMutex(&KbQueueMutex);
		printf("PsychHID-ERROR: Tried to stop processing on keyboard queue for deviceIndex %i, but releasing device failed!\n", deviceIndex);
		PsychErrorExitMsg(PsychError_user, "Stopping keyboard queue failed!");
    }

    // Disable state-change event notifications:
    if (DI_OK != kb->SetEventNotification(NULL)) {
        PsychUnlockMutex(&KbQueueMutex);
		printf("PsychHID-ERROR: Tried to stop processing on keyboard queue for deviceIndex %i, but disabling device state notifications failed!\n", deviceIndex);
		PsychErrorExitMsg(PsychError_user, "Stopping keyboard queue failed!");
    }

	// Mark queue logically stopped:
	psychHIDKbQueueActive[deviceIndex] = FALSE;
    
	PsychUnlockMutex(&KbQueueMutex);
    
	// Was this the last active queue?
	queueActive = FALSE;
	for (i = 0; i < PSYCH_HID_MAX_DEVICES; i++) {
		queueActive |= psychHIDKbQueueActive[i];
	}
    
	// If more queues are active then we're done:
	if (queueActive) return;
    
	// No more active queues. Shutdown the common processing thread:
	PsychLockMutex(&KbQueueMutex);
    
	KbQueueThreadTerminate = TRUE;
    
	// Done.
	PsychUnlockMutex(&KbQueueMutex);
    
	// Shutdown the thread, wait for its termination:
	PsychDeleteThread(&KbQueueThread);
	KbQueueThreadTerminate = FALSE;
    
	// printf("DEBUG: THREAD JOINED.\n"); fflush(NULL);
    
	return;
}

void PsychHIDOSKbQueueStart(int deviceIndex)
{
    LPDIRECTINPUTDEVICE8 kb;
    DIPROPDWORD  dipdw;
	psych_bool queueActive;
	int i;
    
	if (deviceIndex < 0) {
		deviceIndex = PsychHIDGetDefaultKbQueueDevice();
		// Ok, deviceIndex now contains our default keyboard to use - The first suitable keyboard.
	}
    
	if ((deviceIndex < 0) || (deviceIndex >= ndevices)) {
		// Out of range index:
		PsychErrorExitMsg(PsychError_user, "Invalid keyboard 'deviceIndex' specified. No such device!");
	}
    
	// Does Keyboard queue for this deviceIndex already exist?
	if (NULL == psychHIDKbQueueFirstPress[deviceIndex]) {
		// No. Bad bad...
		printf("PsychHID-ERROR: Tried to start processing on non-existent keyboard queue for deviceIndex %i! Call KbQueueCreate first!\n", deviceIndex);
		PsychErrorExitMsg(PsychError_user, "Invalid keyboard 'deviceIndex' specified. No queue for that device yet!");
	}
    
	// Keyboard queue already stopped? Then we ain't nothing to do:
	if (psychHIDKbQueueActive[deviceIndex]) return;
    
	// Queue is inactive. Start it:
    
	// Will this be the first active queue, ie., aren't there any queues running so far?
	queueActive = FALSE;
	for (i = 0; i < PSYCH_HID_MAX_DEVICES; i++) {
		queueActive |= psychHIDKbQueueActive[i];
	}
    
	PsychLockMutex(&KbQueueMutex);
    
	// Clear out current state for this queue:
	memset(psychHIDKbQueueFirstPress[deviceIndex]   , 0, (256 * sizeof(double)));
	memset(psychHIDKbQueueFirstRelease[deviceIndex] , 0, (256 * sizeof(double)));
	memset(psychHIDKbQueueLastPress[deviceIndex]    , 0, (256 * sizeof(double)));
	memset(psychHIDKbQueueLastRelease[deviceIndex]  , 0, (256 * sizeof(double)));
    
	// Setup event mask, so events from our associated xinput device
	// get enqueued in our event queue:
    kb = GetXDevice(deviceIndex);    
    if (DI_OK != kb->SetDataFormat(&c_dfDIKeyboard)) {
        PsychUnlockMutex(&KbQueueMutex);
		printf("PsychHID-ERROR: Tried to start processing on keyboard queue for deviceIndex %i, but setting dataformat failed!\n", deviceIndex);
		PsychErrorExitMsg(PsychError_user, "Starting keyboard queue failed!");
    }
    
    // Set device event buffer size to 256 elements:
    dipdw.diph.dwSize = sizeof(DIPROPDWORD);
    dipdw.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    dipdw.diph.dwObj = 0;
    dipdw.diph.dwHow = DIPH_DEVICE;
    dipdw.dwData = 256;
    
    if (DI_OK != kb->SetProperty(DIPROP_BUFFERSIZE, &dipdw.diph)) {
        PsychUnlockMutex(&KbQueueMutex);
		printf("PsychHID-ERROR: Tried to start processing on keyboard queue for deviceIndex %i, but setting buffersize on device failed!\n", deviceIndex);
		PsychErrorExitMsg(PsychError_user, "Starting keyboard queue failed!");
    }
    
    // Enable state-change event notifications:
    if (DI_OK != kb->SetEventNotification(hEvent)) {
        PsychUnlockMutex(&KbQueueMutex);
		printf("PsychHID-ERROR: Tried to start processing on keyboard queue for deviceIndex %i, but setting device state notifications failed!\n", deviceIndex);
		PsychErrorExitMsg(PsychError_user, "Starting keyboard queue failed!");
    }
    
    if (DI_OK != kb->Acquire()) {
        PsychUnlockMutex(&KbQueueMutex);
		printf("PsychHID-ERROR: Tried to start processing on keyboard queue for deviceIndex %i, but acquiring device failed!\n", deviceIndex);
		PsychErrorExitMsg(PsychError_user, "Starting keyboard queue failed!");
    }
    
	// Mark this queue as logically started:
	psychHIDKbQueueActive[deviceIndex] = TRUE;
    
	// Queue started.
	PsychUnlockMutex(&KbQueueMutex);
    
	// If other queues are already active then we're done:
	if (queueActive) return;
    
	// No other active queues. We are the first one.
    
	// Start the common processing thread for all queues:
	PsychLockMutex(&KbQueueMutex);
	KbQueueThreadTerminate = FALSE;
    
	if (PsychCreateThread(&KbQueueThread, NULL, KbQueueWorkerThreadMain, NULL)) {
		// We are soo screwed:
        
		// Cleanup the mess:
		psychHIDKbQueueActive[deviceIndex] = FALSE;
		PsychUnlockMutex(&KbQueueMutex);
        
		// Whine a little bit:
		printf("PsychHID-ERROR: Start of keyboard queue processing failed!\n");
		PsychErrorExitMsg(PsychError_system, "Creation of keyboard queue background processing thread failed!");
	}
    
	// Up and running, we're done!
	PsychUnlockMutex(&KbQueueMutex);
    
	return;
}

void PsychHIDOSKbQueueFlush(int deviceIndex)
{
    LPDIRECTINPUTDEVICE8 kb;
	HRESULT rc;
    DWORD dwItems = INFINITE;
    
	if (deviceIndex < 0) {
		deviceIndex = PsychHIDGetDefaultKbQueueDevice();
		// Ok, deviceIndex now contains our default keyboard to use - The first suitable keyboard.
	}
    
	if ((deviceIndex < 0) || (deviceIndex >= ndevices)) {
		// Out of range index:
		PsychErrorExitMsg(PsychError_user, "Invalid keyboard 'deviceIndex' specified. No such device!");
	}
    
	// Does Keyboard queue for this deviceIndex already exist?
	if (NULL == psychHIDKbQueueFirstPress[deviceIndex]) {
		// No. Bad bad...
		printf("PsychHID-ERROR: Tried to flush non-existent keyboard queue for deviceIndex %i! Call KbQueueCreate first!\n", deviceIndex);
		PsychErrorExitMsg(PsychError_user, "Invalid keyboard 'deviceIndex' specified. No queue for that device yet!");
	}
    
    kb = GetXDevice(deviceIndex);    
    
	// Clear out current state for this queue:
	PsychLockMutex(&KbQueueMutex);
    
    // Flush device buffer:
    rc = kb->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), NULL, &dwItems, 0);
    
    // Clear our buffer:
	memset(psychHIDKbQueueFirstPress[deviceIndex]   , 0, (256 * sizeof(double)));
	memset(psychHIDKbQueueFirstRelease[deviceIndex] , 0, (256 * sizeof(double)));
	memset(psychHIDKbQueueLastPress[deviceIndex]    , 0, (256 * sizeof(double)));
	memset(psychHIDKbQueueLastRelease[deviceIndex]  , 0, (256 * sizeof(double)));
    
	PsychUnlockMutex(&KbQueueMutex);
    
    return;
}

void PsychHIDOSKbQueueCheck(int deviceIndex)
{
	double *hasKeyBeenDownOutput, *firstPressTimeOutput, *firstReleaseTimeOutput, *lastPressTimeOutput, *lastReleaseTimeOutput;
	psych_bool isFirstPressSpecified, isFirstReleaseSpecified, isLastPressSpecified, isLastReleaseSpecified;
	int i;
    
	if (deviceIndex < 0) {
		deviceIndex = PsychHIDGetDefaultKbQueueDevice();
		// Ok, deviceIndex now contains our default keyboard to use - The first suitable keyboard.
	}
    
	if ((deviceIndex < 0) || (deviceIndex >= ndevices)) {
		// Out of range index:
		PsychErrorExitMsg(PsychError_user, "Invalid keyboard 'deviceIndex' specified. No such device!");
	}
    
	// Does Keyboard queue for this deviceIndex already exist?
	if (NULL == psychHIDKbQueueFirstPress[deviceIndex]) {
		// No. Bad bad...
		printf("PsychHID-ERROR: Tried to check non-existent keyboard queue for deviceIndex %i! Call KbQueueCreate first!\n", deviceIndex);
		PsychErrorExitMsg(PsychError_user, "Invalid keyboard 'deviceIndex' specified. No queue for that device yet!");
	}
    
	// Allocate output
	PsychAllocOutDoubleArg(1, kPsychArgOptional, &hasKeyBeenDownOutput);
	isFirstPressSpecified = PsychAllocOutDoubleMatArg(2, kPsychArgOptional, 1, 256, 1, &firstPressTimeOutput);
	isFirstReleaseSpecified = PsychAllocOutDoubleMatArg(3, kPsychArgOptional, 1, 256, 1, &firstReleaseTimeOutput);
	isLastPressSpecified = PsychAllocOutDoubleMatArg(4, kPsychArgOptional, 1, 256, 1, &lastPressTimeOutput);
	isLastReleaseSpecified = PsychAllocOutDoubleMatArg(5, kPsychArgOptional, 1, 256, 1, &lastReleaseTimeOutput);
    
	// Initialize output
	if(isFirstPressSpecified) memset((void*) firstPressTimeOutput, 0, sizeof(double) * 256);
	if(isFirstReleaseSpecified) memset((void*) firstReleaseTimeOutput, 0, sizeof(double) * 256);
	if(isLastPressSpecified) memset((void*) lastPressTimeOutput, 0, sizeof(double) * 256);
	if(isLastReleaseSpecified) memset((void*) lastReleaseTimeOutput, 0, sizeof(double) * 256);
	
	*hasKeyBeenDownOutput=0;
    
	// Compute and assign output:
	PsychLockMutex(&KbQueueMutex);
    
	for (i = 0; i < 256; i++) {
		double lastRelease  = psychHIDKbQueueLastRelease[deviceIndex][i];
		double lastPress    = psychHIDKbQueueLastPress[deviceIndex][i];
		double firstRelease = psychHIDKbQueueFirstRelease[deviceIndex][i];
		double firstPress   = psychHIDKbQueueFirstPress[deviceIndex][i];
        
		if (firstPress) {
			*hasKeyBeenDownOutput=1;
			if(isFirstPressSpecified) firstPressTimeOutput[i] = firstPress;
			psychHIDKbQueueFirstPress[deviceIndex][i] = 0;
		}
        
		if (firstRelease) {
			if(isFirstReleaseSpecified) firstReleaseTimeOutput[i] = firstRelease;
			psychHIDKbQueueFirstRelease[deviceIndex][i] = 0;
		}
        
		if (lastPress) {
			if(isLastPressSpecified) lastPressTimeOutput[i] = lastPress;
			psychHIDKbQueueLastPress[deviceIndex][i] = 0;
		}
        
		if (lastRelease) {
			if(isLastReleaseSpecified) lastReleaseTimeOutput[i] = lastRelease;
			psychHIDKbQueueLastRelease[deviceIndex][i] = 0;
		}
	}
    
	PsychUnlockMutex(&KbQueueMutex);
    
    return;
}

void PsychHIDOSKbTriggerWait(int deviceIndex, int numScankeys, int* scanKeys)
{
    int keyMask[256];
    int i;
    double t;
    
    if (deviceIndex < 0) {
        deviceIndex = PsychHIDGetDefaultKbQueueDevice();
        // Ok, deviceIndex now contains our default keyboard to use - The first suitable keyboard.
    }
    
    if ((deviceIndex < 0) || (deviceIndex >= ndevices)) {
        // Out of range index:
        PsychErrorExitMsg(PsychError_user, "Invalid keyboard 'deviceIndex' specified. No such device!");
    }
    
    if(psychHIDKbQueueFirstPress[deviceIndex]) PsychErrorExitMsg(PsychError_user, "A queue for this device is already running, you must call KbQueueRelease() before invoking KbTriggerWait.");
    
    // Create a keyboard queue for this deviceIndex:
    memset(&keyMask[0], 0, sizeof(keyMask));
    for (i = 0; i < numScankeys; i++) {
        if (scanKeys[i] < 1 || scanKeys[i] > 256) PsychErrorExitMsg(PsychError_user, "Invalid entry for triggerKey specified. Not in valid range 1 - 256!");
        keyMask[scanKeys[i] - 1] = 1;
    }
    
    // Create keyboard queue with proper mask:
    PsychHIDOSKbQueueCreate(deviceIndex, 256, &keyMask[0]);
    PsychHIDOSKbQueueStart(deviceIndex);
    
    PsychLockMutex(&KbQueueMutex);
    
    // Scan for trigger key:
    while (1) {
        // Wait until something changes in a keyboard queue:
        PsychWaitCondition(&KbQueueCondition, &KbQueueMutex);
        
        // Check if our queue had one of the dedicated trigger keys pressed:
        for (i = 0; i < numScankeys; i++) {
            // Break out of scan loop if key pressed:
            if (psychHIDKbQueueFirstPress[deviceIndex][scanKeys[i] - 1] != 0) break;
        }
        
        // Triggerkey pressed?
        if ((i < numScankeys) && (psychHIDKbQueueFirstPress[deviceIndex][scanKeys[i] - 1] != 0)) break;
        
        // No change for our trigger keys. Repeat scan loop.
    }
    
    // Timestamp:
    PsychGetAdjustedPrecisionTimerSeconds(&t);
    
    // Done. Release the lock:
    PsychUnlockMutex(&KbQueueMutex);
    
    // Stop and release the queue:
    PsychHIDOSKbQueueStop(deviceIndex);
    PsychHIDOSKbQueueRelease(deviceIndex);
    
    // Return timestamp:
    PsychCopyOutDoubleArg(1, kPsychArgOptional, t);
    
    return;
}

#ifdef __cplusplus
}
#endif