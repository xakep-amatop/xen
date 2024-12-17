// Code generated by gengotypes.py. DO NOT EDIT.
// source: libxl_types.idl

package xenlight

type Error int
const(
ErrorNonspecific Error = -1
ErrorVersion Error = -2
ErrorFail Error = -3
ErrorNi Error = -4
ErrorNomem Error = -5
ErrorInval Error = -6
ErrorBadfail Error = -7
ErrorGuestTimedout Error = -8
ErrorTimedout Error = -9
ErrorNoparavirt Error = -10
ErrorNotReady Error = -11
ErrorOseventRegFail Error = -12
ErrorBufferfull Error = -13
ErrorUnknownChild Error = -14
ErrorLockFail Error = -15
ErrorJsonConfigEmpty Error = -16
ErrorDeviceExists Error = -17
ErrorCheckpointDevopsDoesNotMatch Error = -18
ErrorCheckpointDeviceNotSupported Error = -19
ErrorVnumaConfigInvalid Error = -20
ErrorDomainNotfound Error = -21
ErrorAborted Error = -22
ErrorNotfound Error = -23
ErrorDomainDestroyed Error = -24
ErrorFeatureRemoved Error = -25
ErrorProtocolErrorQmp Error = -26
ErrorUnknownQmpError Error = -27
ErrorQmpGenericError Error = -28
ErrorQmpCommandNotFound Error = -29
ErrorQmpDeviceNotActive Error = -30
ErrorQmpDeviceNotFound Error = -31
ErrorQemuApi Error = -32
)

type DomainType int
const(
DomainTypeInvalid DomainType = -1
DomainTypeHvm DomainType = 1
DomainTypePv DomainType = 2
DomainTypePvh DomainType = 3
)

type RdmReserveStrategy int
const(
RdmReserveStrategyIgnore RdmReserveStrategy = 0
RdmReserveStrategyHost RdmReserveStrategy = 1
)

type RdmReservePolicy int
const(
RdmReservePolicyInvalid RdmReservePolicy = -1
RdmReservePolicyStrict RdmReservePolicy = 0
RdmReservePolicyRelaxed RdmReservePolicy = 1
)

type ChannelConnection int
const(
ChannelConnectionUnknown ChannelConnection = 0
ChannelConnectionPty ChannelConnection = 1
ChannelConnectionSocket ChannelConnection = 2
)

type DeviceModelVersion int
const(
DeviceModelVersionUnknown DeviceModelVersion = 0
DeviceModelVersionQemuXenTraditional DeviceModelVersion = 1
DeviceModelVersionQemuXen DeviceModelVersion = 2
)

type ConsoleType int
const(
ConsoleTypeUnknown ConsoleType = 0
ConsoleTypeSerial ConsoleType = 1
ConsoleTypePv ConsoleType = 2
ConsoleTypeVuart ConsoleType = 3
)

type DiskFormat int
const(
DiskFormatUnknown DiskFormat = 0
DiskFormatQcow DiskFormat = 1
DiskFormatQcow2 DiskFormat = 2
DiskFormatVhd DiskFormat = 3
DiskFormatRaw DiskFormat = 4
DiskFormatEmpty DiskFormat = 5
DiskFormatQed DiskFormat = 6
)

type DiskBackend int
const(
DiskBackendUnknown DiskBackend = 0
DiskBackendPhy DiskBackend = 1
DiskBackendTap DiskBackend = 2
DiskBackendQdisk DiskBackend = 3
DiskBackendStandalone DiskBackend = 4
)

type DiskSpecification int
const(
DiskSpecificationUnknown DiskSpecification = 0
DiskSpecificationXen DiskSpecification = 1
DiskSpecificationVirtio DiskSpecification = 2
)

type DiskTransport int
const(
DiskTransportUnknown DiskTransport = 0
DiskTransportMmio DiskTransport = 1
)

type NicType int
const(
NicTypeUnknown NicType = 0
NicTypeVifIoemu NicType = 1
NicTypeVif NicType = 2
)

type P9Type int
const(
P9TypeUnknown P9Type = 0
P9TypeQemu P9Type = 1
P9TypeXen9Pfsd P9Type = 2
)

type ActionOnShutdown int
const(
ActionOnShutdownDestroy ActionOnShutdown = 1
ActionOnShutdownRestart ActionOnShutdown = 2
ActionOnShutdownRestartRename ActionOnShutdown = 3
ActionOnShutdownPreserve ActionOnShutdown = 4
ActionOnShutdownCoredumpDestroy ActionOnShutdown = 5
ActionOnShutdownCoredumpRestart ActionOnShutdown = 6
ActionOnShutdownSoftReset ActionOnShutdown = 7
)

type Trigger int
const(
TriggerUnknown Trigger = 0
TriggerPower Trigger = 1
TriggerSleep Trigger = 2
TriggerNmi Trigger = 3
TriggerInit Trigger = 4
TriggerReset Trigger = 5
TriggerS3Resume Trigger = 6
)

type TscMode int
const(
TscModeDefault TscMode = 0
TscModeAlwaysEmulate TscMode = 1
TscModeNative TscMode = 2
TscModeNativeParavirt TscMode = 3
)

type GfxPassthruKind int
const(
GfxPassthruKindDefault GfxPassthruKind = 0
GfxPassthruKindIgd GfxPassthruKind = 1
)

type TimerMode int
const(
TimerModeUnknown TimerMode = -1
TimerModeDelayForMissedTicks TimerMode = 0
TimerModeNoDelayForMissedTicks TimerMode = 1
TimerModeNoMissedTicksPending TimerMode = 2
TimerModeOneMissedTickPending TimerMode = 3
)

type BiosType int
const(
BiosTypeUnknown BiosType = 0
BiosTypeRombios BiosType = 1
BiosTypeSeabios BiosType = 2
BiosTypeOvmf BiosType = 3
)

type Scheduler int
const(
SchedulerUnknown Scheduler = 0
SchedulerSedf Scheduler = 4
SchedulerCredit Scheduler = 5
SchedulerCredit2 Scheduler = 6
SchedulerArinc653 Scheduler = 7
SchedulerRtds Scheduler = 8
SchedulerNull Scheduler = 9
)

type ShutdownReason int
const(
ShutdownReasonUnknown ShutdownReason = -1
ShutdownReasonPoweroff ShutdownReason = 0
ShutdownReasonReboot ShutdownReason = 1
ShutdownReasonSuspend ShutdownReason = 2
ShutdownReasonCrash ShutdownReason = 3
ShutdownReasonWatchdog ShutdownReason = 4
ShutdownReasonSoftReset ShutdownReason = 5
)

type VgaInterfaceType int
const(
VgaInterfaceTypeUnknown VgaInterfaceType = 0
VgaInterfaceTypeCirrus VgaInterfaceType = 1
VgaInterfaceTypeStd VgaInterfaceType = 2
VgaInterfaceTypeNone VgaInterfaceType = 3
VgaInterfaceTypeQxl VgaInterfaceType = 4
)

type VendorDevice int
const(
VendorDeviceNone VendorDevice = 0
VendorDeviceXenserver VendorDevice = 1
)

type ViridianEnlightenment int
const(
ViridianEnlightenmentBase ViridianEnlightenment = 0
ViridianEnlightenmentFreq ViridianEnlightenment = 1
ViridianEnlightenmentTimeRefCount ViridianEnlightenment = 2
ViridianEnlightenmentReferenceTsc ViridianEnlightenment = 3
ViridianEnlightenmentHcallRemoteTlbFlush ViridianEnlightenment = 4
ViridianEnlightenmentApicAssist ViridianEnlightenment = 5
ViridianEnlightenmentCrashCtl ViridianEnlightenment = 6
ViridianEnlightenmentSynic ViridianEnlightenment = 7
ViridianEnlightenmentStimer ViridianEnlightenment = 8
ViridianEnlightenmentHcallIpi ViridianEnlightenment = 9
ViridianEnlightenmentExProcessorMasks ViridianEnlightenment = 10
ViridianEnlightenmentNoVpLimit ViridianEnlightenment = 11
ViridianEnlightenmentCpuHotplug ViridianEnlightenment = 12
)

type Hdtype int
const(
HdtypeIde Hdtype = 1
HdtypeAhci Hdtype = 2
)

type CheckpointedStream int
const(
CheckpointedStreamNone CheckpointedStream = 0
CheckpointedStreamRemus CheckpointedStream = 1
CheckpointedStreamColo CheckpointedStream = 2
)

type VuartType int
const(
VuartTypeUnknown VuartType = 0
VuartTypeSbsaUart VuartType = 1
)

type VkbBackend int
const(
VkbBackendUnknown VkbBackend = 0
VkbBackendQemu VkbBackend = 1
VkbBackendLinux VkbBackend = 2
)

type VirtioTransport int
const(
VirtioTransportUnknown VirtioTransport = 0
VirtioTransportMmio VirtioTransport = 1
)

type Passthrough int
const(
PassthroughDefault Passthrough = 0
PassthroughDisabled Passthrough = 1
PassthroughEnabled Passthrough = 2
PassthroughSyncPt Passthrough = 3
PassthroughSharePt Passthrough = 4
)

type IoportRange struct {
First uint32
Number uint32
}

type IomemRange struct {
Start uint64
Number uint64
Gfn uint64
}

type VgaInterfaceInfo struct {
Kind VgaInterfaceType
}

type VncInfo struct {
Enable Defbool
Listen string
Passwd string
Display int
Findunused Defbool
}

type SpiceInfo struct {
Enable Defbool
Port int
TlsPort int
Host string
DisableTicketing Defbool
Passwd string
AgentMouse Defbool
Vdagent Defbool
ClipboardSharing Defbool
Usbredirection int
ImageCompression string
StreamingVideo string
}

type SdlInfo struct {
Enable Defbool
Opengl Defbool
Display string
Xauthority string
}

type Dominfo struct {
Uuid Uuid
Domid Domid
Ssidref uint32
SsidLabel string
Running bool
Blocked bool
Paused bool
Shutdown bool
Dying bool
NeverStop bool
ShutdownReason ShutdownReason
OutstandingMemkb uint64
CurrentMemkb uint64
SharedMemkb uint64
PagedMemkb uint64
MaxMemkb uint64
CpuTime uint64
VcpuMaxId uint32
VcpuOnline uint32
Cpupool uint32
GpaddrBits byte
DomainType DomainType
}

type Cpupoolinfo struct {
Poolid uint32
PoolName string
Sched Scheduler
NDom uint32
Cpumap Bitmap
}

type Channelinfo struct {
Backend string
BackendId uint32
Frontend string
FrontendId uint32
Devid Devid
State int
Evtch int
Rref int
Connection ChannelConnection
ConnectionUnion ChannelinfoConnectionUnion
}

type ChannelinfoConnectionUnion interface {
isChannelinfoConnectionUnion()
}

type ChannelinfoConnectionUnionPty struct {
Path string
}

func (x ChannelinfoConnectionUnionPty) isChannelinfoConnectionUnion(){}

type Vminfo struct {
Uuid Uuid
Domid Domid
}

type VersionInfo struct {
XenVersionMajor int
XenVersionMinor int
XenVersionExtra string
Compiler string
CompileBy string
CompileDomain string
CompileDate string
Capabilities string
Changeset string
VirtStart uint64
Pagesize int
Commandline string
BuildId string
}

type SmbiosType int
const(
SmbiosTypeBiosVendor SmbiosType = 1
SmbiosTypeBiosVersion SmbiosType = 2
SmbiosTypeSystemManufacturer SmbiosType = 3
SmbiosTypeSystemProductName SmbiosType = 4
SmbiosTypeSystemVersion SmbiosType = 5
SmbiosTypeSystemSerialNumber SmbiosType = 6
SmbiosTypeBaseboardManufacturer SmbiosType = 7
SmbiosTypeBaseboardProductName SmbiosType = 8
SmbiosTypeBaseboardVersion SmbiosType = 9
SmbiosTypeBaseboardSerialNumber SmbiosType = 10
SmbiosTypeBaseboardAssetTag SmbiosType = 11
SmbiosTypeBaseboardLocationInChassis SmbiosType = 12
SmbiosTypeEnclosureManufacturer SmbiosType = 13
SmbiosTypeEnclosureSerialNumber SmbiosType = 14
SmbiosTypeEnclosureAssetTag SmbiosType = 15
SmbiosTypeBatteryManufacturer SmbiosType = 16
SmbiosTypeBatteryDeviceName SmbiosType = 17
SmbiosTypeOem SmbiosType = 18
)

type Smbios struct {
Key SmbiosType
Value string
}

type DomainCreateInfo struct {
Type DomainType
Hap Defbool
Oos Defbool
Ssidref uint32
SsidLabel string
Name string
Domid Domid
Uuid Uuid
Xsdata KeyValueList
Platformdata KeyValueList
Poolid uint32
PoolName string
RunHotplugScripts Defbool
DriverDomain Defbool
Passthrough Passthrough
XendSuspendEvtchnCompat Defbool
}

type DomainRestoreParams struct {
CheckpointedStream int
StreamVersion uint32
ColoProxyScript string
UserspaceColoProxy Defbool
}

type SchedParams struct {
Vcpuid int
Weight int
Cap int
Period int
Extratime int
Budget int
}

type VcpuSchedParams struct {
Sched Scheduler
Vcpus []SchedParams
}

type DomainSchedParams struct {
Sched Scheduler
Weight int
Cap int
Period int
Budget int
Extratime int
Slice int
Latency int
}

type VnodeInfo struct {
Memkb uint64
Distances []uint32
Pnode uint32
Vcpus Bitmap
}

type GicVersion int
const(
GicVersionDefault GicVersion = 0
GicVersionV2 GicVersion = 32
GicVersionV3 GicVersion = 48
)

type TeeType int
const(
TeeTypeNone TeeType = 0
TeeTypeOptee TeeType = 1
TeeTypeFfa TeeType = 2
)

type SveType int
const(
SveTypeHw SveType = -1
SveTypeDisabled SveType = 0
SveType128 SveType = 128
SveType256 SveType = 256
SveType384 SveType = 384
SveType512 SveType = 512
SveType640 SveType = 640
SveType768 SveType = 768
SveType896 SveType = 896
SveType1024 SveType = 1024
SveType1152 SveType = 1152
SveType1280 SveType = 1280
SveType1408 SveType = 1408
SveType1536 SveType = 1536
SveType1664 SveType = 1664
SveType1792 SveType = 1792
SveType1920 SveType = 1920
SveType2048 SveType = 2048
)

type RdmReserve struct {
Strategy RdmReserveStrategy
Policy RdmReservePolicy
}

type Altp2MMode int
const(
Altp2MModeDisabled Altp2MMode = 0
Altp2MModeMixed Altp2MMode = 1
Altp2MModeExternal Altp2MMode = 2
Altp2MModeLimited Altp2MMode = 3
)

type DomainBuildInfo struct {
MaxVcpus int
AvailVcpus Bitmap
Cpumap Bitmap
Nodemap Bitmap
VcpuHardAffinity []Bitmap
VcpuSoftAffinity []Bitmap
NumaPlacement Defbool
TscMode TscMode
MaxMemkb uint64
TargetMemkb uint64
VideoMemkb uint64
ShadowMemkb uint64
IommuMemkb uint64
RtcTimeoffset uint32
ExecSsidref uint32
ExecSsidLabel string
Localtime Defbool
DisableMigrate Defbool
Cpuid CpuidPolicyList
BlkdevStart string
VnumaNodes []VnodeInfo
MaxGrantFrames uint32
MaxMaptrackFrames uint32
MaxGrantVersion int
DeviceModelVersion DeviceModelVersion
DeviceModelStubdomain Defbool
StubdomainMemkb uint64
StubdomainKernel string
StubdomainCmdline string
StubdomainRamdisk string
DeviceModel string
DeviceModelSsidref uint32
DeviceModelSsidLabel string
DeviceModelUser string
Extra StringList
ExtraPv StringList
ExtraHvm StringList
SchedParams DomainSchedParams
Ioports []IoportRange
Irqs []uint32
Iomem []IomemRange
LlcColors []uint32
ClaimMode Defbool
EventChannels uint32
Kernel string
Cmdline string
Ramdisk string
DeviceTree string
Acpi Defbool
Bootloader string
BootloaderArgs StringList
BootloaderRestrict Defbool
BootloaderUser string
TimerMode TimerMode
NestedHvm Defbool
Apic Defbool
DmRestrict Defbool
Tee TeeType
Type DomainType
TypeUnion DomainBuildInfoTypeUnion
ArchArm struct {
GicVersion GicVersion
Vuart VuartType
SveVl SveType
NrSpis uint32
}
ArchX86 struct {
MsrRelaxed Defbool
}
Altp2M Altp2MMode
VmtraceBufKb int
Vpmu Defbool
}

type DomainBuildInfoTypeUnion interface {
isDomainBuildInfoTypeUnion()
}

type DomainBuildInfoTypeUnionHvm struct {
Firmware string
Bios BiosType
Pae Defbool
Apic Defbool
Acpi Defbool
AcpiS3 Defbool
AcpiS4 Defbool
AcpiLaptopSlate Defbool
Nx Defbool
Viridian Defbool
ViridianEnable Bitmap
ViridianDisable Bitmap
Timeoffset string
Hpet Defbool
VptAlign Defbool
MmioHoleMemkb uint64
TimerMode TimerMode
NestedHvm Defbool
Altp2M Defbool
SystemFirmware string
SmbiosFirmware string
Smbios []Smbios
AcpiFirmware string
Hdtype Hdtype
Nographic Defbool
Vga VgaInterfaceInfo
Vnc VncInfo
Keymap string
Sdl SdlInfo
Spice SpiceInfo
GfxPassthru Defbool
GfxPassthruKind GfxPassthruKind
Serial string
Boot string
Usb Defbool
Usbversion int
Usbdevice string
VkbDevice Defbool
Soundhw string
XenPlatformPci Defbool
UsbdeviceList StringList
VendorDevice VendorDevice
MsVmGenid MsVmGenid
SerialList StringList
Rdm RdmReserve
RdmMemBoundaryMemkb uint64
McaCaps uint64
Pirq Defbool
}

func (x DomainBuildInfoTypeUnionHvm) isDomainBuildInfoTypeUnion(){}

type DomainBuildInfoTypeUnionPv struct {
Kernel string
SlackMemkb uint64
Bootloader string
BootloaderArgs StringList
Cmdline string
Ramdisk string
Features string
E820Host Defbool
}

func (x DomainBuildInfoTypeUnionPv) isDomainBuildInfoTypeUnion(){}

type DomainBuildInfoTypeUnionPvh struct {
Pvshim Defbool
PvshimPath string
PvshimCmdline string
PvshimExtra string
}

func (x DomainBuildInfoTypeUnionPvh) isDomainBuildInfoTypeUnion(){}

type DeviceVfb struct {
BackendDomid Domid
BackendDomname string
Devid Devid
Vnc VncInfo
Sdl SdlInfo
Keymap string
}

type DeviceVkb struct {
BackendDomid Domid
BackendDomname string
Devid Devid
BackendType VkbBackend
UniqueId string
FeatureDisableKeyboard bool
FeatureDisablePointer bool
FeatureAbsPointer bool
FeatureRawPointer bool
FeatureMultiTouch bool
Width uint32
Height uint32
MultiTouchWidth uint32
MultiTouchHeight uint32
MultiTouchNumContacts uint32
}

type DeviceVirtio struct {
BackendDomid Domid
BackendDomname string
Type string
Transport VirtioTransport
GrantUsage Defbool
Devid Devid
Irq uint32
Base uint64
}

type DeviceDisk struct {
BackendDomid Domid
BackendDomname string
PdevPath string
Vdev string
Backend DiskBackend
Format DiskFormat
Script string
Removable int
Readwrite int
IsCdrom int
DirectIoSafe bool
DiscardEnable Defbool
Specification DiskSpecification
Transport DiskTransport
Irq uint32
Base uint64
ColoEnable Defbool
ColoRestoreEnable Defbool
ColoHost string
ColoPort int
ColoExport string
ActiveDisk string
HiddenDisk string
Trusted Defbool
GrantUsage Defbool
}

type DeviceNic struct {
BackendDomid Domid
BackendDomname string
Devid Devid
Mtu int
Vlan string
Model string
Mac Mac
Ip string
Bridge string
Ifname string
Script string
Nictype NicType
RateBytesPerInterval uint64
RateIntervalUsecs uint32
Gatewaydev string
ColoftForwarddev string
ColoSockMirrorId string
ColoSockMirrorIp string
ColoSockMirrorPort string
ColoSockComparePriInId string
ColoSockComparePriInIp string
ColoSockComparePriInPort string
ColoSockCompareSecInId string
ColoSockCompareSecInIp string
ColoSockCompareSecInPort string
ColoSockCompareNotifyId string
ColoSockCompareNotifyIp string
ColoSockCompareNotifyPort string
ColoSockRedirector0Id string
ColoSockRedirector0Ip string
ColoSockRedirector0Port string
ColoSockRedirector1Id string
ColoSockRedirector1Ip string
ColoSockRedirector1Port string
ColoSockRedirector2Id string
ColoSockRedirector2Ip string
ColoSockRedirector2Port string
ColoFilterMirrorQueue string
ColoFilterMirrorOutdev string
ColoFilterRedirector0Queue string
ColoFilterRedirector0Indev string
ColoFilterRedirector0Outdev string
ColoFilterRedirector1Queue string
ColoFilterRedirector1Indev string
ColoFilterRedirector1Outdev string
ColoComparePriIn string
ColoCompareSecIn string
ColoCompareOut string
ColoCompareNotifyDev string
ColoSockSecRedirector0Id string
ColoSockSecRedirector0Ip string
ColoSockSecRedirector0Port string
ColoSockSecRedirector1Id string
ColoSockSecRedirector1Ip string
ColoSockSecRedirector1Port string
ColoFilterSecRedirector0Queue string
ColoFilterSecRedirector0Indev string
ColoFilterSecRedirector0Outdev string
ColoFilterSecRedirector1Queue string
ColoFilterSecRedirector1Indev string
ColoFilterSecRedirector1Outdev string
ColoFilterSecRewriter0Queue string
ColoCheckpointHost string
ColoCheckpointPort string
Trusted Defbool
}

type DevicePci struct {
Func byte
Dev byte
Bus byte
Domain int
Vdevfn uint32
VfuncMask uint32
Msitranslate bool
PowerMgmt bool
Permissive bool
Seize bool
RdmPolicy RdmReservePolicy
Name string
}

type DeviceRdm struct {
Start uint64
Size uint64
Policy RdmReservePolicy
}

type UsbctrlType int
const(
UsbctrlTypeAuto UsbctrlType = 0
UsbctrlTypePv UsbctrlType = 1
UsbctrlTypeDevicemodel UsbctrlType = 2
UsbctrlTypeQusb UsbctrlType = 3
)

type UsbdevType int
const(
UsbdevTypeHostdev UsbdevType = 1
)

type DeviceUsbctrl struct {
Type UsbctrlType
Devid Devid
Version int
Ports int
BackendDomid Domid
BackendDomname string
}

type DeviceUsbdev struct {
Ctrl Devid
Port int
Type UsbdevType
TypeUnion DeviceUsbdevTypeUnion
}

type DeviceUsbdevTypeUnion interface {
isDeviceUsbdevTypeUnion()
}

type DeviceUsbdevTypeUnionHostdev struct {
Hostbus byte
Hostaddr byte
}

func (x DeviceUsbdevTypeUnionHostdev) isDeviceUsbdevTypeUnion(){}

type DeviceDtdev struct {
Path string
}

type DeviceVtpm struct {
BackendDomid Domid
BackendDomname string
Devid Devid
Uuid Uuid
}

type DeviceP9 struct {
BackendDomid Domid
BackendDomname string
Tag string
Path string
SecurityModel string
Devid Devid
Type P9Type
MaxSpace int
MaxFiles int
MaxOpenFiles int
AutoDelete bool
}

type DevicePvcallsif struct {
BackendDomid Domid
BackendDomname string
Devid Devid
}

type DeviceChannel struct {
BackendDomid Domid
BackendDomname string
Devid Devid
Name string
Connection ChannelConnection
ConnectionUnion DeviceChannelConnectionUnion
}

type DeviceChannelConnectionUnion interface {
isDeviceChannelConnectionUnion()
}

type DeviceChannelConnectionUnionSocket struct {
Path string
}

func (x DeviceChannelConnectionUnionSocket) isDeviceChannelConnectionUnion(){}

type ConnectorParam struct {
UniqueId string
Width uint32
Height uint32
}

type DeviceVdispl struct {
BackendDomid Domid
BackendDomname string
Devid Devid
BeAlloc bool
Connectors []ConnectorParam
}

type VsndPcmFormat int
const(
VsndPcmFormatS8 VsndPcmFormat = 1
VsndPcmFormatU8 VsndPcmFormat = 2
VsndPcmFormatS16Le VsndPcmFormat = 3
VsndPcmFormatS16Be VsndPcmFormat = 4
VsndPcmFormatU16Le VsndPcmFormat = 5
VsndPcmFormatU16Be VsndPcmFormat = 6
VsndPcmFormatS24Le VsndPcmFormat = 7
VsndPcmFormatS24Be VsndPcmFormat = 8
VsndPcmFormatU24Le VsndPcmFormat = 9
VsndPcmFormatU24Be VsndPcmFormat = 10
VsndPcmFormatS32Le VsndPcmFormat = 11
VsndPcmFormatS32Be VsndPcmFormat = 12
VsndPcmFormatU32Le VsndPcmFormat = 13
VsndPcmFormatU32Be VsndPcmFormat = 14
VsndPcmFormatF32Le VsndPcmFormat = 15
VsndPcmFormatF32Be VsndPcmFormat = 16
VsndPcmFormatF64Le VsndPcmFormat = 17
VsndPcmFormatF64Be VsndPcmFormat = 18
VsndPcmFormatIec958SubframeLe VsndPcmFormat = 19
VsndPcmFormatIec958SubframeBe VsndPcmFormat = 20
VsndPcmFormatMuLaw VsndPcmFormat = 21
VsndPcmFormatALaw VsndPcmFormat = 22
VsndPcmFormatImaAdpcm VsndPcmFormat = 23
VsndPcmFormatMpeg VsndPcmFormat = 24
VsndPcmFormatGsm VsndPcmFormat = 25
)

type VsndParams struct {
SampleRates []uint32
SampleFormats []VsndPcmFormat
ChannelsMin uint32
ChannelsMax uint32
BufferSize uint32
}

type VsndStreamType int
const(
VsndStreamTypeP VsndStreamType = 1
VsndStreamTypeC VsndStreamType = 2
)

type VsndStream struct {
UniqueId string
Type VsndStreamType
Params VsndParams
}

type VsndPcm struct {
Name string
Params VsndParams
Streams []VsndStream
}

type DeviceVsnd struct {
BackendDomid Domid
BackendDomname string
Devid Devid
ShortName string
LongName string
Params VsndParams
Pcms []VsndPcm
}

type DomainConfig struct {
CInfo DomainCreateInfo
BInfo DomainBuildInfo
Disks []DeviceDisk
Nics []DeviceNic
Pcidevs []DevicePci
Rdms []DeviceRdm
Dtdevs []DeviceDtdev
Vfbs []DeviceVfb
Vkbs []DeviceVkb
Virtios []DeviceVirtio
Vtpms []DeviceVtpm
P9S []DeviceP9
Pvcallsifs []DevicePvcallsif
Vdispls []DeviceVdispl
Vsnds []DeviceVsnd
Channels []DeviceChannel
Usbctrls []DeviceUsbctrl
Usbdevs []DeviceUsbdev
OnPoweroff ActionOnShutdown
OnReboot ActionOnShutdown
OnWatchdog ActionOnShutdown
OnCrash ActionOnShutdown
OnSoftReset ActionOnShutdown
}

type Diskinfo struct {
Backend string
BackendId uint32
Frontend string
FrontendId uint32
Devid Devid
State int
Evtch int
Rref int
}

type Nicinfo struct {
Backend string
BackendId uint32
Frontend string
FrontendId uint32
Devid Devid
State int
Evtch int
RrefTx int
RrefRx int
}

type Vtpminfo struct {
Backend string
BackendId uint32
Frontend string
FrontendId uint32
Devid Devid
State int
Evtch int
Rref int
Uuid Uuid
}

type Usbctrlinfo struct {
Type UsbctrlType
Devid Devid
Version int
Ports int
Backend string
BackendId uint32
Frontend string
FrontendId uint32
State int
Evtch int
RefUrb int
RefConn int
}

type Vcpuinfo struct {
Vcpuid uint32
Cpu uint32
Online bool
Blocked bool
Running bool
VcpuTime uint64
Cpumap Bitmap
CpumapSoft Bitmap
}

type Physinfo struct {
ThreadsPerCore uint32
CoresPerSocket uint32
MaxCpuId uint32
NrCpus uint32
CpuKhz uint32
TotalPages uint64
FreePages uint64
ScrubPages uint64
OutstandingPages uint64
SharingFreedPages uint64
SharingUsedFrames uint64
MaxPossibleMfn uint64
NrNodes uint32
HwCap Hwcap
CapHvm bool
CapPv bool
CapHvmDirectio bool
CapHap bool
CapShadow bool
CapIommuHapPtShare bool
CapVmtrace bool
CapVpmu bool
CapGnttabV1 bool
CapGnttabV2 bool
ArchCapabilities uint32
}

type Connectorinfo struct {
UniqueId string
Width uint32
Height uint32
ReqEvtch int
ReqRref int
EvtEvtch int
EvtRref int
}

type Vdisplinfo struct {
Backend string
BackendId uint32
Frontend string
FrontendId uint32
Devid Devid
State int
BeAlloc bool
Connectors []Connectorinfo
}

type Streaminfo struct {
ReqEvtch int
ReqRref int
}

type Pcminfo struct {
Streams []Streaminfo
}

type Vsndinfo struct {
Backend string
BackendId uint32
Frontend string
FrontendId uint32
Devid Devid
State int
Pcms []Pcminfo
}

type Vkbinfo struct {
Backend string
BackendId uint32
Frontend string
FrontendId uint32
Devid Devid
State int
Evtch int
Rref int
}

type Numainfo struct {
Size uint64
Free uint64
Dists []uint32
}

type Cputopology struct {
Core uint32
Socket uint32
Node uint32
}

type Pcitopology struct {
Seg uint16
Bus byte
Devfn byte
Node uint32
}

type SchedCreditParams struct {
TsliceMs int
RatelimitUs int
VcpuMigrDelayUs int
}

type SchedCredit2Params struct {
RatelimitUs int
}

type DomainRemusInfo struct {
Interval int
AllowUnsafe Defbool
Blackhole Defbool
Compression Defbool
Netbuf Defbool
Netbufscript string
Diskbuf Defbool
Colo Defbool
UserspaceColoProxy Defbool
}

type EventType int
const(
EventTypeDomainShutdown EventType = 1
EventTypeDomainDeath EventType = 2
EventTypeDiskEject EventType = 3
EventTypeOperationComplete EventType = 4
EventTypeDomainCreateConsoleAvailable EventType = 5
)

type Event struct {
Link EvLink
Domid Domid
Domuuid Uuid
ForUser uint64
Type EventType
TypeUnion EventTypeUnion
}

type EventTypeUnion interface {
isEventTypeUnion()
}

type EventTypeUnionDomainShutdown struct {
ShutdownReason byte
}

func (x EventTypeUnionDomainShutdown) isEventTypeUnion(){}

type EventTypeUnionDiskEject struct {
Vdev string
Disk DeviceDisk
}

func (x EventTypeUnionDiskEject) isEventTypeUnion(){}

type EventTypeUnionOperationComplete struct {
Rc int
}

func (x EventTypeUnionOperationComplete) isEventTypeUnion(){}

type PsrCmtType int
const(
PsrCmtTypeCacheOccupancy PsrCmtType = 1
PsrCmtTypeTotalMemCount PsrCmtType = 2
PsrCmtTypeLocalMemCount PsrCmtType = 3
)

type PsrCbmType int
const(
PsrCbmTypeUnknown PsrCbmType = 0
PsrCbmTypeL3Cbm PsrCbmType = 1
PsrCbmTypeL3CbmCode PsrCbmType = 2
PsrCbmTypeL3CbmData PsrCbmType = 3
PsrCbmTypeL2Cbm PsrCbmType = 4
PsrCbmTypeMbaThrtl PsrCbmType = 5
)

type PsrCatInfo struct {
Id uint32
CosMax uint32
CbmLen uint32
CdpEnabled bool
}

type PsrFeatType int
const(
PsrFeatTypeCat PsrFeatType = 1
PsrFeatTypeMba PsrFeatType = 2
)

type PsrHwInfo struct {
Id uint32
Type PsrFeatType
TypeUnion PsrHwInfoTypeUnion
}

type PsrHwInfoTypeUnion interface {
isPsrHwInfoTypeUnion()
}

type PsrHwInfoTypeUnionCat struct {
CosMax uint32
CbmLen uint32
CdpEnabled bool
}

func (x PsrHwInfoTypeUnionCat) isPsrHwInfoTypeUnion(){}

type PsrHwInfoTypeUnionMba struct {
CosMax uint32
ThrtlMax uint32
Linear bool
}

func (x PsrHwInfoTypeUnionMba) isPsrHwInfoTypeUnion(){}

