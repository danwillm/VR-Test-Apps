#include <iostream>
#include <vector>

#include "openxr/openxr.h"
#include <thread>

#define b_qualify_xr(x) do {                                            \
        XrResult ret = x;                                               \
        if(XR_FAILED(ret)) {                                            \
            std::cout << "[QualifyXR] failed: " << #x << " " << ret << std::endl;   \
            return false;                                               \
        }                                                               \
    } while(0)                                                          \


PFN_xrEnumerateInteractionRenderModelIdsEXT xrEnumerateInteractionRenderModelIdsEXT;
PFN_xrInteractionRenderModelGetTopLevelUserPathEXT xrInteractionRenderModelGetTopLevelUserPathEXT;
PFN_xrCreateRenderModelEXT xrCreateRenderModelEXT;
PFN_xrDestroyRenderModelEXT xrDestroyRenderModelEXT;

int main() {
	system("pause");

	std::vector<const char*> enabledExtensions = {
			XR_MND_HEADLESS_EXTENSION_NAME,
			XR_HTCX_VIVE_TRACKER_INTERACTION_EXTENSION_NAME,
			XR_EXT_UUID_EXTENSION_NAME,
			XR_EXT_INTERACTION_RENDER_MODEL_EXTENSION_NAME,
			XR_EXT_IRM_TOPLEVEL_PATH_EXTENSION_NAME,
			XR_EXT_RENDER_MODEL_EXTENSION_NAME,
	};

	XrInstance instance;
	{
		XrApplicationInfo applicationInfo = {
				.applicationName = "rendermodeltest",
				.applicationVersion = 1,
				.engineName = "",
				.engineVersion = 1,
				.apiVersion = XR_MAKE_VERSION(1, 0, 0),
		};
		XrInstanceCreateInfo instanceCreateInfo = {
				.type = XR_TYPE_INSTANCE_CREATE_INFO,
				.next = nullptr,
				.applicationInfo = applicationInfo,
				.enabledApiLayerCount = 0,
				.enabledExtensionCount = (uint32_t)enabledExtensions.size(),
				.enabledExtensionNames = enabledExtensions.data(),
		};
		if (XrResult result = xrCreateInstance(&instanceCreateInfo, &instance)) {
			std::cout << "Failed to create instance: " << result << std::endl;
			return 1;
		}
	}

#define xr_get_proc(instance, name) do {                                                    \
        b_qualify_xr(xrGetInstanceProcAddr(instance, #name, (PFN_xrVoidFunction *) &name)); \
    } while(0)      

	xr_get_proc(instance, xrEnumerateInteractionRenderModelIdsEXT);
	xr_get_proc(instance, xrInteractionRenderModelGetTopLevelUserPathEXT);
	xr_get_proc(instance, xrCreateRenderModelEXT);
	xr_get_proc(instance, xrDestroyRenderModelEXT);


	XrInstanceProperties instanceProperties = { XR_TYPE_INSTANCE_PROPERTIES };
	if (xrGetInstanceProperties(instance, &instanceProperties) == XR_SUCCESS) {
		std::cout << "Runtime name: " << instanceProperties.runtimeName << "\n";
		std::cout << "Runtime version: "
			<< XR_VERSION_MAJOR(instanceProperties.runtimeVersion) << "."
			<< XR_VERSION_MAJOR(instanceProperties.runtimeVersion) << "."
			<< XR_VERSION_MAJOR(instanceProperties.runtimeVersion) << "\n";
	}

	XrSystemId systemId;
	XrSystemGetInfo systemGetInfo = {
			.type = XR_TYPE_SYSTEM_GET_INFO,
			.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY,
	};
	if (XrResult result = xrGetSystem(instance, &systemGetInfo, &systemId)) {
		std::cout << "Failed to get system: " << result << std::endl;
		return 1;
	}

	XrSession session;
	{
		XrSessionCreateInfo sessionCreateInfo = {
				.type = XR_TYPE_SESSION_CREATE_INFO,
				.next = nullptr, //graphicsBinding here if not headless
				.systemId = systemId,
		};
		if (XrResult result = xrCreateSession(instance, &sessionCreateInfo, &session)) {
			std::cout << "Failed to create system: " << result << std::endl;
			return 1;
		}
	}

	XrActionSet actionSet;
	XrActionSetCreateInfo actionSetCreateInfo = {
			.type = XR_TYPE_ACTION_SET_CREATE_INFO,
			.actionSetName = "actionsettest",
			.localizedActionSetName = "Action Set Test",
			.priority = 0,
	};
	if (XrResult result = xrCreateActionSet(instance, &actionSetCreateInfo, &actionSet)) {
		std::cout << "Failed to create action set: " << result << std::endl;
		return 1;
	}

	XrPath trackerChest;
	if (XrResult result = xrStringToPath(instance, "/user/vive_tracker_htcx/role/chest", &trackerChest)) {
		std::cout << "String to path error: " << result << std::endl;
		return 1;
	}

	XrAction action1;
	{
		XrActionCreateInfo actionCreateInfo = {
				.type = XR_TYPE_ACTION_CREATE_INFO,
				.actionName = "action1",
				.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT,
				.countSubactionPaths = 1,
				.subactionPaths = &trackerChest,
				.localizedActionName = "Action Test 1",
		};
		if (XrResult result = xrCreateAction(actionSet, &actionCreateInfo, &action1)) {
			std::cout << "Failed to create action: " << result << std::endl;
			return 1;
		}
	}

	XrAction action2;
	{
		XrActionCreateInfo actionCreateInfo = {
				.type = XR_TYPE_ACTION_CREATE_INFO,
				.actionName = "action2",
				.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT,
				.countSubactionPaths = 1,
				.subactionPaths = &trackerChest,
				.localizedActionName = "Action Test 2",
		};
		if (XrResult result = xrCreateAction(actionSet, &actionCreateInfo, &action2)) {
			std::cout << "Failed to create action " << result << std::endl;
			return 1;
		}
	}

	XrPath viveTrackerInteractionProfilePath;
	if (XrResult result = xrStringToPath(instance, "/interaction_profiles/htc/vive_tracker_htcx", &viveTrackerInteractionProfilePath)) {
		std::cout << "String to path error: " << result << std::endl;
		return 1;
	}

	XrPath indexInteractionProfilePath;
	if (XrResult result = xrStringToPath(instance, "/interaction_profiles/valve/index_controller", &indexInteractionProfilePath)) {
		std::cout << "String to path error: " << result << std::endl;
		return 1;
	}


	XrPath binding1;
	if (XrResult result = xrStringToPath(instance, "/user/vive_tracker_htcx/role/chest/input/squeeze/click", &binding1)) {
		std::cout << "String to path error: " << result << std::endl;
		return 1;
	}

	XrPath binding2;
	if (XrResult result = xrStringToPath(instance, "/user/hand/right/input/a/click", &binding2)) {
		std::cout << "String to path error: " << result << std::endl;
		return 1;
	}

	std::vector<XrActionSuggestedBinding> viveTrackerSuggestedBindings = {
			{
					.action = action1,
					.binding = binding1
			},
	};

	std::vector<XrActionSuggestedBinding> indexSuggestedBindings = {
		{
			.action = action2,
			.binding = binding2,
		}
	};

	XrInteractionProfileSuggestedBinding viveTrackerBindings = {
			.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
			.interactionProfile = viveTrackerInteractionProfilePath,
			.countSuggestedBindings = (uint32_t)viveTrackerSuggestedBindings.size(),
			.suggestedBindings = viveTrackerSuggestedBindings.data(),
	};
	if (XrResult result = xrSuggestInteractionProfileBindings(instance, &viveTrackerBindings)) {
		std::cout << "Failed to suggest bindings: " << result << std::endl;
		return 1;
	}

	XrInteractionProfileSuggestedBinding indexBindings = {
		.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
		.interactionProfile = indexInteractionProfilePath,
		.countSuggestedBindings = (uint32_t)indexSuggestedBindings.size(),
		.suggestedBindings = indexSuggestedBindings.data(),
	};
	if (XrResult result = xrSuggestInteractionProfileBindings(instance, &indexBindings)) {
		std::cout << "Failed to suggest bindings: " << result << std::endl;
		return 1;
	}

	bool bSessionFocused = false;

	XrEventDataBuffer event = { XR_TYPE_EVENT_DATA_BUFFER };
	while (true) {
		XrResult pollResult = xrPollEvent(instance, &event);
		while (pollResult == XR_SUCCESS) {
			switch (event.type) {
			case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
				const XrEventDataSessionStateChanged& sessionStateChanged = *reinterpret_cast<XrEventDataSessionStateChanged*>(&event);

				switch (sessionStateChanged.state) {
				case XR_SESSION_STATE_READY: {
					XrSessionActionSetsAttachInfo attachInfo = {
						.type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO,
						.countActionSets = 1,
						.actionSets = &actionSet,
					};
					if (XrResult result2 = xrAttachSessionActionSets(session, &attachInfo)) {
						std::cout << "Failed to attach action sets: " << result2 << std::endl;
						return 1;
					}

					XrSessionBeginInfo beginInfo = {
						.type = XR_TYPE_SESSION_BEGIN_INFO,
						.next = nullptr,
					};
					if (XrResult result2 = xrBeginSession(session, &beginInfo))
					{
						std::cout << "Failed to begin session: " << result2 << std::endl;
					}

					break;
				}

				case XR_SESSION_STATE_FOCUSED: {
					bSessionFocused = true;
					break;
				}
				}

				break;
			}
			}

			pollResult = xrPollEvent(instance, &event);
		}

		if (bSessionFocused)
		{
			XrActiveActionSet activeActionSet = {
				.actionSet = actionSet,
				.subactionPath = XR_NULL_PATH
			};

			XrActionsSyncInfo actionSyncInfo = {
				.type = XR_TYPE_ACTIONS_SYNC_INFO,
				.countActiveActionSets = 1,
				.activeActionSets = &activeActionSet
			};

			if (XrResult result = xrSyncActions(session, &actionSyncInfo)) {
				std::cout << "Failed to sync actions: " << result << std::endl;
				return 1;
			}

			XrInteractionRenderModelIdsEnumerateInfoEXT enumerateInfo = {
				.type = XR_TYPE_INTERACTION_RENDER_MODEL_IDS_ENUMERATE_INFO_EXT,
			};

			uint32_t unRenderModelCount = 0;
			std::vector<XrRenderModelIdEXT> vRenderModelIds;
			xrEnumerateInteractionRenderModelIdsEXT(session, &enumerateInfo, 0, &unRenderModelCount, nullptr);

			vRenderModelIds.resize(unRenderModelCount);
			xrEnumerateInteractionRenderModelIdsEXT(session, &enumerateInfo, vRenderModelIds.size(), &unRenderModelCount, vRenderModelIds.data());

			for (int i = 0; i < unRenderModelCount; i++)
			{
				XrRenderModelEXT renderModel;

				XrRenderModelCreateInfoEXT renderModelCreateInfo = {
				.type = XR_TYPE_RENDER_MODEL_CREATE_INFO_EXT,
					.renderModelId = vRenderModelIds[i],
				};
				xrCreateRenderModelEXT(session, &renderModelCreateInfo, &renderModel);

				XrPath topLevelUserPath;
				xrInteractionRenderModelGetTopLevelUserPathEXT(renderModel, &topLevelUserPath);

				uint32_t unBuffOutput;
				char path[XR_MAX_PATH_LENGTH];
				xrPathToString(instance, topLevelUserPath, XR_MAX_PATH_LENGTH, &unBuffOutput, path);

				std::cout << "Id: " << i << " Path: " << path << std::endl;
				xrDestroyRenderModelEXT(renderModel);
			}

		}

		std::this_thread::sleep_for(std::chrono::milliseconds(500));
	}

}