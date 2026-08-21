#include "Cafe/HW/Espresso/WupsBackendAbi.h"

#include <algorithm>

namespace
{
	constexpr std::array kExports{
		WupsBackendExportDescriptor{"WUPSLoadPluginAsDataByPath", WupsBackendExportId::LoadPluginAsDataByPath, WupsSymbolKind::Function},
		WupsBackendExportDescriptor{"WUPSLoadPluginAsDataByBuffer", WupsBackendExportId::LoadPluginAsDataByBuffer, WupsSymbolKind::Function},
		WupsBackendExportDescriptor{"WUPSLoadPluginAsData", WupsBackendExportId::LoadPluginAsData, WupsSymbolKind::Function},
		WupsBackendExportDescriptor{"WUPSLoadAndLinkByDataHandle", WupsBackendExportId::LoadAndLinkByDataHandle, WupsSymbolKind::Function},
		WupsBackendExportDescriptor{"WUPSDeletePluginData", WupsBackendExportId::DeletePluginData, WupsSymbolKind::Function},
		WupsBackendExportDescriptor{"WUPSGetPluginMetaInformation", WupsBackendExportId::GetPluginMetaInformation, WupsSymbolKind::Function},
		WupsBackendExportDescriptor{"WUPSGetPluginMetaInformationByPath", WupsBackendExportId::GetPluginMetaInformationByPath, WupsSymbolKind::Function},
		WupsBackendExportDescriptor{"WUPSGetPluginMetaInformationByBuffer", WupsBackendExportId::GetPluginMetaInformationByBuffer, WupsSymbolKind::Function},
		WupsBackendExportDescriptor{"WUPSGetMetaInformation", WupsBackendExportId::GetMetaInformation, WupsSymbolKind::Function},
		WupsBackendExportDescriptor{"WUPSGetLoadedPlugins", WupsBackendExportId::GetLoadedPlugins, WupsSymbolKind::Function},
		WupsBackendExportDescriptor{"WUPSGetPluginDataForContainerHandles", WupsBackendExportId::GetPluginDataForContainerHandles, WupsSymbolKind::Function},
		WupsBackendExportDescriptor{"WUPSGetAPIVersion", WupsBackendExportId::GetApiVersion, WupsSymbolKind::Function},
		WupsBackendExportDescriptor{"WUPSGetNumberOfLoadedPlugins", WupsBackendExportId::GetNumberOfLoadedPlugins, WupsSymbolKind::Function},
		WupsBackendExportDescriptor{"WUPSGetSectionInformationForPlugin", WupsBackendExportId::GetSectionInformationForPlugin, WupsSymbolKind::Function},
		WupsBackendExportDescriptor{"WUPSWillReloadPluginsOnNextLaunch", WupsBackendExportId::WillReloadPluginsOnNextLaunch, WupsSymbolKind::Function},
		WupsBackendExportDescriptor{"WUPSGetSectionMemoryAddresses", WupsBackendExportId::GetSectionMemoryAddresses, WupsSymbolKind::Function},
		WupsBackendExportDescriptor{"WUPSGetPluginMetaInformationByPathEx", WupsBackendExportId::GetPluginMetaInformationByPathEx, WupsSymbolKind::Function},
		WupsBackendExportDescriptor{"WUPSGetPluginMetaInformationByBufferEx", WupsBackendExportId::GetPluginMetaInformationByBufferEx, WupsSymbolKind::Function},
	};
}

std::span<const WupsBackendExportDescriptor> WupsBackendExportDescriptors()
{
	return kExports;
}

const WupsBackendExportDescriptor* FindWupsBackendExport(
	std::string_view name, WupsSymbolKind kind)
{
	const auto found = std::ranges::find_if(kExports,
											[name, kind](const auto& export_) {
												return export_.name == name && export_.kind == kind;
											});
	return found == kExports.end() ? nullptr : &*found;
}
