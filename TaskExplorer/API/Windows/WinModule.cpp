#include "stdafx.h"
#include "../../GUI/TaskExplorer.h"
#include "WinModule.h"

#include "ProcessHacker.h"


#include <QtWin>

CWinModule::CWinModule(bool IsSubsystemProcess, QObject *parent) 
	: CModuleInfo(parent) 
{
	m_IsSubsystemProcess = IsSubsystemProcess;
	m_EntryPoint = NULL;
    m_Flags = 0;
    m_Type = 0;
    m_LoadReason = 0;
    m_LoadCount = 0;
	m_ImageCharacteristics = 0;
	m_ImageDllCharacteristics = 0;

	m_VerifyResult = VrUnknown;
}

CWinModule::~CWinModule()
{
}

bool CWinModule::InitStaticData(struct _PH_MODULE_INFO* module, quint64 ProcessHandle)
{
	QReadLocker Locker(&m_Mutex);

	m_FileName = CastPhString(module->FileName, false);
	m_ModuleName = CastPhString(module->Name, false);

    m_BaseAddress = (quint64)module->BaseAddress;
    m_EntryPoint = (quint64)module->EntryPoint;
    m_Size = module->Size;
    m_Flags = module->Flags;
    m_Type = module->Type;
    m_LoadReason = module->LoadReason;
    m_LoadCount = module->LoadCount;
    m_LoadTime = QDateTime::fromTime_t((int64_t)module->LoadTime.QuadPart / 10000000ULL - 11644473600ULL);
    m_ParentBaseAddress = (quint64)module->ParentBaseAddress;

	if (m_IsSubsystemProcess)
    {
        // HACK: Update the module type. (TODO: Move into PhEnumGenericModules) (dmex)
        m_Type = PH_MODULE_TYPE_ELF_MAPPED_IMAGE;
    }
    else
    {
        // Fix up the load count. If this is not an ordinary DLL or kernel module, set the load count to 0.
        if (m_Type != PH_MODULE_TYPE_MODULE &&
            m_Type != PH_MODULE_TYPE_WOW64_MODULE &&
            m_Type != PH_MODULE_TYPE_KERNEL_MODULE)
        {
            m_LoadCount = 0;
        }
    }

	if (m_Type == PH_MODULE_TYPE_MODULE ||
        m_Type == PH_MODULE_TYPE_WOW64_MODULE ||
        m_Type == PH_MODULE_TYPE_MAPPED_IMAGE ||
        m_Type == PH_MODULE_TYPE_KERNEL_MODULE)
    {
        PH_REMOTE_MAPPED_IMAGE remoteMappedImage;
        PPH_READ_VIRTUAL_MEMORY_CALLBACK readVirtualMemoryCallback;

        if (m_Type == PH_MODULE_TYPE_KERNEL_MODULE)
            readVirtualMemoryCallback = KphReadVirtualMemoryUnsafe;
        else
            readVirtualMemoryCallback = NtReadVirtualMemory;

        // Note:
        // On Windows 7 the LDRP_IMAGE_NOT_AT_BASE flag doesn't appear to be used
        // anymore. Instead we'll check ImageBase in the image headers. We read this in
        // from the process' memory because:
        //
        // 1. It (should be) faster than opening the file and mapping it in, and
        // 2. It contains the correct original image base relocated by ASLR, if present.

        m_Flags &= ~LDRP_IMAGE_NOT_AT_BASE;

        if (NT_SUCCESS(PhLoadRemoteMappedImageEx((HANDLE)ProcessHandle, &m_BaseAddress, readVirtualMemoryCallback, &remoteMappedImage)))
        {
            ULONG_PTR imageBase = 0;
            ULONG entryPoint = 0;

            m_ImageTimeDateStamp = QDateTime::fromTime_t(remoteMappedImage.NtHeaders->FileHeader.TimeDateStamp);
            m_ImageCharacteristics = remoteMappedImage.NtHeaders->FileHeader.Characteristics;

            if (remoteMappedImage.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
            {
                PIMAGE_OPTIONAL_HEADER32 optionalHeader = (PIMAGE_OPTIONAL_HEADER32)&remoteMappedImage.NtHeaders->OptionalHeader;

                imageBase = (ULONG_PTR)optionalHeader->ImageBase;
                entryPoint = optionalHeader->AddressOfEntryPoint;
                m_ImageDllCharacteristics = optionalHeader->DllCharacteristics;
            }
            else if (remoteMappedImage.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
            {
                PIMAGE_OPTIONAL_HEADER64 optionalHeader = (PIMAGE_OPTIONAL_HEADER64)&remoteMappedImage.NtHeaders->OptionalHeader;

                imageBase = (ULONG_PTR)optionalHeader->ImageBase;
                entryPoint = optionalHeader->AddressOfEntryPoint;
                m_ImageDllCharacteristics = optionalHeader->DllCharacteristics;
            }

            if (imageBase != (ULONG_PTR)m_BaseAddress)
                m_Flags |= LDRP_IMAGE_NOT_AT_BASE;

            if (entryPoint != 0)
                m_EntryPoint = (quint64)PTR_ADD_OFFSET(m_BaseAddress, entryPoint);

            PhUnloadRemoteMappedImage(&remoteMappedImage);
        }
    }

	FILE_NETWORK_OPEN_INFORMATION networkOpenInfo;
    if (NT_SUCCESS(PhQueryFullAttributesFileWin32((PWSTR)m_FileName.toStdWString().c_str(), &networkOpenInfo)))
    {
		m_ModificationTime = QDateTime::fromTime_t((int64_t)networkOpenInfo.LastWriteTime.QuadPart / 10000000ULL - 11644473600ULL);
        m_FileSize = networkOpenInfo.EndOfFile.QuadPart;
    }

    if (m_Type != PH_MODULE_TYPE_ELF_MAPPED_IMAGE)
    {
		this->InitAsyncData(m_FileName);
    }

	return true;
}

void CWinModule::ClearControlFlowGuardEnabled()
{
	QReadLocker Locker(&m_Mutex);
	m_ImageDllCharacteristics &= ~IMAGE_DLLCHARACTERISTICS_GUARD_CF;
}

bool CWinModule::UpdateDynamicData(struct _PH_MODULE_INFO* module)
{
	QReadLocker Locker(&m_Mutex);

	BOOLEAN modified = FALSE;

	/*
            

            if (m_JustProcessed)
                modified = TRUE;

            m_JustProcessed = FALSE;

            if (m_LoadCount != module->LoadCount)
            {
                m_LoadCount = module->LoadCount;
                modified = TRUE;
            }
	*/
	return modified;
}

void CWinModule::InitAsyncData(const QString& FileName, const QString& PackageFullName)
{
	QReadLocker Locker(&m_Mutex);

	m_FileName = FileName;

	QVariantMap Params;
	Params["FileName"] = FileName;
	Params["PackageFullName"] = PackageFullName;
	Params["IsSubsystemProcess"] = m_IsSubsystemProcess;

	/*if (m_UserName.isEmpty())
		Params["Sid"] = m->Sid;*/

	// Note: this instance of CWinProcess may be deleted before the async proces finishes,
	// so to make things simple and avoid emmory leaks we pass all params and results as a QVariantMap
	// its not the most eficient way but its simple and reliable.

	QFutureWatcher<QVariantMap>* pWatcher = new QFutureWatcher<QVariantMap>(this); // Note: the job will be canceled if the file will be deleted :D
	connect(pWatcher, SIGNAL(resultReadyAt(int)), this, SLOT(OnInitAsyncData(int)));
	connect(pWatcher, SIGNAL(finished()), pWatcher, SLOT(deleteLater()));
	pWatcher->setFuture(QtConcurrent::run(CWinModule::InitAsyncData, Params));
}

// Note: PhInitializeModuleVersionInfoCached does not look thread safe, so we have to guard it.
QMutex g_ModuleVersionInfoCachedMutex;

QVariantMap CWinModule::InitAsyncData(QVariantMap Params)
{
	QVariantMap Result;

	PPH_STRING FileName = CastQString(Params["FileName"].toString());
	PPH_STRING PackageFullName = CastQString(Params["PackageFullName"].toString());
	BOOLEAN IsSubsystemProcess = Params["IsSubsystemProcess"].toBool();

	PH_IMAGE_VERSION_INFO VersionInfo = { NULL, NULL, NULL, NULL };

	// PhpProcessQueryStage1 Begin
	NTSTATUS status;

	if (FileName && !IsSubsystemProcess)
	{
		HICON SmallIcon;
		HICON LargeIcon;
		if (!PhExtractIcon(FileName->Buffer, &LargeIcon, &SmallIcon))
		{
			LargeIcon = NULL;
			SmallIcon = NULL;
		}

		if (SmallIcon)
		{
			Result["SmallIcon"] = QtWin::fromHICON(SmallIcon);
			DestroyIcon(SmallIcon);
		}

		if (LargeIcon)
		{
			Result["LargeIcon"] = QtWin::fromHICON(LargeIcon);
			DestroyIcon(LargeIcon);
		}

		// Version info.
		QMutexLocker Lock(&g_ModuleVersionInfoCachedMutex);
		PhInitializeImageVersionInfoCached(&VersionInfo, FileName, FALSE);
	}

	/*if (Params.contains("Sid"))
		Result["UserName"] = GetSidFullNameCached(Params["Sid"].toByteArray());*/
	// PhpProcessQueryStage1 End

	// PhpProcessQueryStage2 Begin
	if (FileName && !IsSubsystemProcess)
	{
		NTSTATUS status;

		VERIFY_RESULT VerifyResult;
		PPH_STRING VerifySignerName;
		ulong ImportFunctions;
		ulong ImportModules;

		BOOLEAN IsPacked;

		VerifyResult = PhVerifyFileCached(FileName, PackageFullName, &VerifySignerName, FALSE);


		status = PhIsExecutablePacked(FileName->Buffer, &IsPacked, &ImportModules, &ImportFunctions);

		// If we got an Module-related error, the Module is packed.
		if (status == STATUS_INVALID_IMAGE_NOT_MZ || status == STATUS_INVALID_IMAGE_FORMAT || status == STATUS_ACCESS_VIOLATION)
		{
			IsPacked = TRUE;
			ImportModules = ULONG_MAX;
			ImportFunctions = ULONG_MAX;
		}

		Result["VerifyResult"] = (int)VerifyResult;
		Result["VerifySignerName"] = CastPhString(VerifySignerName);
		Result["IsPacked"] = IsPacked;
		Result["ImportFunctions"] = (quint32)ImportFunctions;
		Result["ImportModules"] = (quint32)ImportModules;
	}

	if (/*PhEnableLinuxSubsystemSupport &&*/ FileName && IsSubsystemProcess)
	{
		QMutexLocker Lock(&g_ModuleVersionInfoCachedMutex);
		PhInitializeImageVersionInfoCached(&VersionInfo, FileName, TRUE);
	}
	// PhpProcessQueryStage2 End

	QVariantMap Infos;
	Infos["CompanyName"] = CastPhString(VersionInfo.CompanyName);
	Infos["Description"] = CastPhString(VersionInfo.FileDescription);
	Infos["FileVersion"] = CastPhString(VersionInfo.FileVersion);
	Infos["ProductName"] = CastPhString(VersionInfo.ProductName);

	Result["Infos"] = Infos;

	PhDereferenceObject(FileName);
	PhDereferenceObject(PackageFullName);

	return Result;
}

void CWinModule::OnInitAsyncData(int Index)
{
	QFutureWatcher<QVariantMap>* pWatcher = (QFutureWatcher<QVariantMap>*)sender();

	QVariantMap Result = pWatcher->resultAt(Index);

	QWriteLocker Locker(&m_Mutex);

	/*if (Result.contains("UserName"))
		m_UserName = Result["UserName"].toString();*/

	m_SmallIcon = Result["SmallIcon"].value<QPixmap>();
	m_LargeIcon = Result["LargeIcon"].value<QPixmap>();

	m_FileDetails.clear();
	QVariantMap Infos = Result["Infos"].toMap();
	foreach(const QString& Key, Infos.keys())
		m_FileDetails[Key] = Infos[Key].toString();

	m_VerifyResult = (EVerifyResult)Result["VerifyResult"].toInt();
	m_VerifySignerName = Result["VerifySignerName"].toString();

	emit AsyncDataDone(Result["IsPacked"].toBool(), Result["ImportFunctions"].toUInt(), Result["ImportModules"].toUInt());
}

QString CWinModule::GetTypeString() const
{
	QReadLocker Locker(&m_Mutex);

    switch (m_Type)
    {
    case PH_MODULE_TYPE_MODULE:
        return tr("DLL");
    case PH_MODULE_TYPE_MAPPED_FILE:
        return tr("Mapped file");
    case PH_MODULE_TYPE_MAPPED_IMAGE:
    case PH_MODULE_TYPE_ELF_MAPPED_IMAGE:
        return tr("Mapped image");
    case PH_MODULE_TYPE_WOW64_MODULE:
        return tr("WOW64 DLL");
    case PH_MODULE_TYPE_KERNEL_MODULE:
        return tr("Kernel module");
    default:	
		return tr("Unknown");
    }
}

QString CWinModule::GetVerifyResultString() const
{
	QReadLocker Locker(&m_Mutex);
	
	switch (m_VerifyResult)
	{
	case VrTrusted:			return tr("Trusted");
	case VrNoSignature:		return tr("Un signed");
	case VrExpired:
	case VrRevoked:
	case VrDistrust:
	case VrBadSignature:	return tr("Not trusted");
	default:				return tr("Unknown");
	}
}

QString CWinModule::GetASLRString() const
{
	QReadLocker Locker(&m_Mutex);

	if (m_ImageDllCharacteristics & IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE)
		return tr("ASLR");
	return QString();
}

QString CWinModule::GetCFGuardString() const
{
	QReadLocker Locker(&m_Mutex);

	if (m_ImageDllCharacteristics & IMAGE_DLLCHARACTERISTICS_GUARD_CF)
		return tr("CF Guard");
	return QString();
}

QString CWinModule::GetLoadReasonString() const
{
	QReadLocker Locker(&m_Mutex);

	if (m_Type == PH_MODULE_TYPE_KERNEL_MODULE)
    {
        return tr("Dynamic");
    }
    else if (m_Type == PH_MODULE_TYPE_MODULE || m_Type == PH_MODULE_TYPE_WOW64_MODULE)
    {
        switch ((_LDR_DLL_LOAD_REASON)m_LoadReason)
        {
        case LoadReasonStaticDependency:			return tr("Static dependency");
        case LoadReasonStaticForwarderDependency:	return tr("Static forwarder dependency");
        case LoadReasonDynamicForwarderDependency:	return tr("Dynamic forwarder dependency");
        case LoadReasonDelayloadDependency:			return tr("Delay load dependency");
        case LoadReasonDynamicLoad:					return tr("Dynamic");
        case LoadReasonAsImageLoad:					return tr("As image");
        case LoadReasonAsDataLoad:					return tr("As data");
        case LoadReasonEnclavePrimary:				return tr("Enclave");
        case LoadReasonEnclaveDependency:			return tr("Enclave dependency");
        default:
			if (WindowsVersion >= WINDOWS_8)
				return tr("Unknown");
			else
				return tr("N/A");
        }
    }
	return QString();
}