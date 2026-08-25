#include <openvr_driver.h>

#include "log.h"
#include "preview_window.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace sample
{
	namespace
	{
		constexpr char k_pchSettingsSection[] = "driver_display_redirect_sample";
		constexpr char k_pchSerialNumberKey[] = "serialNumber";
		constexpr char k_pchModelNumberKey[] = "modelNumber";
		constexpr char k_pchAdapterIndexKey[] = "adapterIndex";
		constexpr char k_pchRefreshRateKey[] = "refreshRate";
		constexpr char k_pchShowPreviewKey[] = "showPreview";
		constexpr char k_pchPreviewWidthKey[] = "previewWidth";
		constexpr char k_pchPreviewHeightKey[] = "previewHeight";
		constexpr char k_pchWaitForVsyncKey[] = "waitForVsync";

		class VirtualVsyncClock
		{
		public:
			void Reset(float flRefreshRate)
			{
				m_flRefreshRate = std::clamp(flRefreshRate, 1.0f, 1000.0f);
				m_period = std::chrono::duration< double >(1.0 / static_cast<double>(m_flRefreshRate));
				m_epoch = Clock::now();
			}

			void WaitForNextVsync() const
			{
				const auto now = Clock::now();
				const auto elapsed = std::chrono::duration< double >(now - m_epoch).count();
				const double period = m_period.count();
				const uint64_t nCompletedFrames = static_cast<uint64_t>(std::max(0.0, std::floor(elapsed / period)));
				const auto next = m_epoch + std::chrono::duration_cast<Clock::duration>(m_period * static_cast<double>(nCompletedFrames + 1));
				std::this_thread::sleep_until(next);
			}

			bool GetTimeSinceLastVsync(float* pfSecondsSinceLastVsync, uint64_t* pulFrameCounter) const
			{
				if (!pfSecondsSinceLastVsync || !pulFrameCounter)
				{
					return false;
				}

				const auto now = Clock::now();
				const double elapsed = std::max(0.0, std::chrono::duration< double >(now - m_epoch).count());
				const double period = m_period.count();
				const uint64_t nFrame = static_cast<uint64_t>(std::floor(elapsed / period));
				const double flLastVsync = static_cast<double>(nFrame) * period;

				*pfSecondsSinceLastVsync = static_cast<float>(elapsed - flLastVsync);
				*pulFrameCounter = nFrame;
				return true;
			}

		private:
			using Clock = std::chrono::steady_clock;

			float m_flRefreshRate = 90.0f;
			Clock::time_point m_epoch = Clock::now();
			std::chrono::duration< double > m_period{ 1.0 / 90.0 };
		};

		class DisplayRedirectDevice final : public vr::ITrackedDeviceServerDriver, public vr::IVRVirtualDisplay
		{
		public:
			DisplayRedirectDevice()
			{
				vr::VRSettings()->GetString(k_pchSettingsSection, k_pchSerialNumberKey,
					m_rchSerialNumber, sizeof(m_rchSerialNumber));
				vr::VRSettings()->GetString(k_pchSettingsSection, k_pchModelNumberKey,
					m_rchModelNumber, sizeof(m_rchModelNumber));

				m_nAdapterIndex = vr::VRSettings()->GetInt32(k_pchSettingsSection, k_pchAdapterIndexKey);
				m_flRefreshRate = vr::VRSettings()->GetFloat(k_pchSettingsSection, k_pchRefreshRateKey);
				m_bShowPreview = vr::VRSettings()->GetBool(k_pchSettingsSection, k_pchShowPreviewKey);
				m_bWaitForVsync = vr::VRSettings()->GetBool(k_pchSettingsSection, k_pchWaitForVsyncKey);

				const int32_t nPreviewWidth = vr::VRSettings()->GetInt32(k_pchSettingsSection, k_pchPreviewWidthKey);
				const int32_t nPreviewHeight = vr::VRSettings()->GetInt32(k_pchSettingsSection, k_pchPreviewHeightKey);

				if (m_rchSerialNumber[0] == '\0')
				{
					strcpy_s(m_rchSerialNumber, "display_redirect_sample_001");
				}
				if (m_rchModelNumber[0] == '\0')
				{
					strcpy_s(m_rchModelNumber, "OpenVR Display Redirect Sample");
				}
				if (m_flRefreshRate <= 0.0f)
				{
					m_flRefreshRate = 90.0f;
				}

				m_vsyncClock.Reset(m_flRefreshRate);

				m_bValid = m_preview.Initialize(
					m_nAdapterIndex,
					nPreviewWidth > 0 ? static_cast<uint32_t>(nPreviewWidth) : 1280u,
					nPreviewHeight > 0 ? static_cast<uint32_t>(nPreviewHeight) : 720u);

				if (m_bValid && m_bShowPreview)
				{
					Log("display_redirect_sample: starting preview before TrackedDeviceAdded/Activate");
					m_preview.Start();
				}

				Log("display_redirect_sample: device created serial=%s model=%s adapterIndex=%d refreshRate=%.3f showPreview=%s waitForVsync=%s",
					m_rchSerialNumber,
					m_rchModelNumber,
					m_nAdapterIndex,
					m_flRefreshRate,
					m_bShowPreview ? "true" : "false",
					m_bWaitForVsync ? "true" : "false");
			}

			~DisplayRedirectDevice()
			{
				m_preview.Stop();
			}

			[[nodiscard]] bool IsValid() const { return m_bValid; }
			[[nodiscard]] const char* GetSerialNumber() const { return m_rchSerialNumber; }

			vr::EVRInitError Activate(uint32_t unObjectId) override
			{
				m_unObjectId = unObjectId;
				m_vsyncClock.Reset(m_flRefreshRate);

				const vr::PropertyContainerHandle_t container =
					vr::VRProperties()->TrackedDeviceToPropertyContainer(unObjectId);

				vr::VRProperties()->SetStringProperty(container, vr::Prop_ModelNumber_String, m_rchModelNumber);
				vr::VRProperties()->SetStringProperty(container, vr::Prop_ManufacturerName_String, "OpenVR sample");
				vr::VRProperties()->SetStringProperty(container, vr::Prop_TrackingSystemName_String, "display_redirect_sample");
				vr::VRProperties()->SetBoolProperty(container, vr::Prop_NeverTracked_Bool, true);
				vr::VRProperties()->SetFloatProperty(container, vr::Prop_DisplayFrequency_Float, m_flRefreshRate);
				vr::VRProperties()->SetFloatProperty(container, vr::Prop_SecondsFromVsyncToPhotons_Float, 0.0f);
				vr::VRProperties()->SetUint64Property(container, vr::Prop_GraphicsAdapterLuid_Uint64, m_preview.GetAdapterLuid());

				Log("display_redirect_sample: Activate objectId=%u, publishing graphics adapter LUID 0x%016llx",
					unObjectId,
					static_cast<unsigned long long>(m_preview.GetAdapterLuid()));

				if (m_bShowPreview)
				{
					m_preview.Start();
				}

				return vr::VRInitError_None;
			}

			void Deactivate() override
			{
				Log("display_redirect_sample: Deactivate");
				m_preview.Stop();
				m_unObjectId = vr::k_unTrackedDeviceIndexInvalid;
			}

			void EnterStandby() override
			{
				Log("display_redirect_sample: EnterStandby");
			}

			void* GetComponent(const char* pchComponentNameAndVersion) override
			{
				if (pchComponentNameAndVersion &&
					std::strcmp(pchComponentNameAndVersion, vr::IVRVirtualDisplay_Version) == 0)
				{
					Log("display_redirect_sample: GetComponent(%s) -> IVRVirtualDisplay", pchComponentNameAndVersion);
					return static_cast<vr::IVRVirtualDisplay*>(this);
				}

				if (pchComponentNameAndVersion)
				{
					Log("display_redirect_sample: GetComponent(%s) -> nullptr", pchComponentNameAndVersion);
				}
				return nullptr;
			}

			void DebugRequest(const char* pchRequest, char* pchResponseBuffer, uint32_t unResponseBufferSize) override
			{
				if (unResponseBufferSize == 0 || !pchResponseBuffer)
				{
					return;
				}

				const char* pchResponse = "display_redirect_sample: ok";
				if (pchRequest && std::strcmp(pchRequest, "status") == 0)
				{
					pchResponse = m_bValid ? "display_redirect_sample: valid" : "display_redirect_sample: invalid";
				}

				strncpy_s(pchResponseBuffer, unResponseBufferSize, pchResponse, _TRUNCATE);
			}

			vr::DriverPose_t GetPose() override
			{
				vr::DriverPose_t pose{};
				pose.deviceIsConnected = true;
				pose.poseIsValid = false;
				pose.result = vr::TrackingResult_Running_OK;
				pose.qWorldFromDriverRotation.w = 1.0;
				pose.qDriverFromHeadRotation.w = 1.0;
				pose.qRotation.w = 1.0;
				return pose;
			}

			void Present(const vr::PresentInfo_t* pPresentInfo, uint32_t unPresentInfoSize) override
			{
				if (!pPresentInfo || unPresentInfoSize < sizeof(vr::PresentInfo_t))
				{
					Log("display_redirect_sample: Present called with invalid PresentInfo (ptr=%p size=%u expected>=%zu)",
						pPresentInfo, unPresentInfoSize, sizeof(vr::PresentInfo_t));
					return;
				}

				m_nLastSubmittedFrameId.store(pPresentInfo->nFrameId, std::memory_order_release);

				const uint64_t nPresentCount = ++m_nPresentCount;
				if (nPresentCount <= 5 || (nPresentCount % 300) == 0)
				{
					Log("display_redirect_sample: Present #%llu frameId=%llu handle=0x%llx vsync=%d compositorVsyncTime=%.6f size=%u",
						static_cast<unsigned long long>(nPresentCount),
						static_cast<unsigned long long>(pPresentInfo->nFrameId),
						static_cast<unsigned long long>(pPresentInfo->backbufferTextureHandle),
						static_cast<int>(pPresentInfo->vsync),
						pPresentInfo->flVSyncTimeInSeconds,
						unPresentInfoSize);
				}

				if (m_bShowPreview)
				{
					m_preview.SubmitFrame(*pPresentInfo);
				}
			}

			void WaitForPresent() override
			{
				const uint64_t nFrameId = m_nLastSubmittedFrameId.load(std::memory_order_acquire);
				if (nFrameId == m_nLastWaitedFrameId)
				{
					return;
				}

				if (m_bWaitForVsync)
				{
					m_vsyncClock.WaitForNextVsync();
				}

				m_nLastWaitedFrameId = nFrameId;
			}

			bool GetTimeSinceLastVsync(float* pfSecondsSinceLastVsync, uint64_t* pulFrameCounter) override
			{
				return m_vsyncClock.GetTimeSinceLastVsync(pfSecondsSinceLastVsync, pulFrameCounter);
			}

		private:
			vr::TrackedDeviceIndex_t m_unObjectId = vr::k_unTrackedDeviceIndexInvalid;
			char m_rchSerialNumber[256]{};
			char m_rchModelNumber[256]{};

			int32_t m_nAdapterIndex = 0;
			float m_flRefreshRate = 90.0f;
			bool m_bShowPreview = true;
			bool m_bWaitForVsync = true;
			bool m_bValid = false;

			PreviewWindow m_preview;
			VirtualVsyncClock m_vsyncClock;

			std::atomic< uint64_t > m_nLastSubmittedFrameId{ 0 };
			uint64_t m_nLastWaitedFrameId = ~uint64_t{ 0 };
			std::atomic< uint64_t > m_nPresentCount{ 0 };
		};

		class ServerDriverProvider final : public vr::IServerTrackedDeviceProvider
		{
		public:
			vr::EVRInitError Init(vr::IVRDriverContext* pDriverContext) override
			{
				VR_INIT_SERVER_DRIVER_CONTEXT(pDriverContext);

				Log("display_redirect_sample: server provider Init");
				m_device = std::make_unique< DisplayRedirectDevice >();
				if (!m_device->IsValid())
				{
					Log("display_redirect_sample: initialization failed; not registering DisplayRedirect device");
					m_device.reset();
					return vr::VRInitError_Driver_Failed;
				}

				const bool bAdded = vr::VRServerDriverHost()->TrackedDeviceAdded(
					m_device->GetSerialNumber(),
					vr::TrackedDeviceClass_DisplayRedirect,
					m_device.get());

				Log("display_redirect_sample: TrackedDeviceAdded(class=DisplayRedirect) -> %s", bAdded ? "true" : "false");
				return bAdded ? vr::VRInitError_None : vr::VRInitError_Driver_Failed;
			}

			void Cleanup() override
			{
				Log("display_redirect_sample: server provider Cleanup");
				m_device.reset();
				VR_CLEANUP_SERVER_DRIVER_CONTEXT();
			}

			const char* const* GetInterfaceVersions() override
			{
				return vr::k_InterfaceVersions;
			}

			void RunFrame() override {}
			bool ShouldBlockStandbyMode() override { return false; }
			void EnterStandby() override {}
			void LeaveStandby() override {}

		private:
			std::unique_ptr< DisplayRedirectDevice > m_device;
		};

		ServerDriverProvider g_serverDriverProvider;
	} // namespace
} // namespace sample

extern "C" __declspec(dllexport) void* HmdDriverFactory(const char* pInterfaceName, int* pReturnCode)
{
	if (pInterfaceName && std::strcmp(pInterfaceName, vr::IServerTrackedDeviceProvider_Version) == 0)
	{
		return &sample::g_serverDriverProvider;
	}

	if (pReturnCode)
	{
		*pReturnCode = vr::VRInitError_Init_InterfaceNotFound;
	}
	return nullptr;
}
