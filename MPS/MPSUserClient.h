#ifndef MPS_USER_CLIENT_H
#define MPS_USER_CLIENT_H
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLocks.h>
class MPSController;
class MPSUserClient : public IOUserClient {
    OSDeclareDefaultStructors(MPSUserClient)
    IOLock *fCallLock;
    MPSController *fController;
public:
    bool initWithTask(task_t task, void *securityId, UInt32 type,
                      OSDictionary *properties) APPLE_KEXT_OVERRIDE;
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
    void free(void) APPLE_KEXT_OVERRIDE;
    IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *arguments,
        IOExternalMethodDispatch *dispatch = NULL, OSObject *target = NULL,
        void *reference = NULL) APPLE_KEXT_OVERRIDE;
};
#endif
