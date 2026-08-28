#include "Common/precompiled.h"

#include "Cemu/CemuExtend/Formats/CemodPackage.h"

#include <boost/asio/ip/address.hpp>
#include <openssl/evp.h>
#include <rapidjson/document.h>
#include <zip.h>

#include <charconv>
#include <array>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace
{

	using Sha256 = std::array<unsigned char, 32>;

	struct ZipCloser
	{
		void operator()(zip_t* value) const
		{
			if (value)
				zip_close(value);
		}
	};
	struct ZipFileCloser
	{
		void operator()(zip_file_t* value) const
		{
			if (value)
				zip_fclose(value);
		}
	};
	struct DigestCloser
	{
		void operator()(EVP_MD_CTX* value) const
		{
			EVP_MD_CTX_free(value);
		}
	};
	struct KeyCloser
	{
		void operator()(EVP_PKEY* value) const
		{
			EVP_PKEY_free(value);
		}
	};

	std::optional<std::string> NormalizedEntryName(std::string_view name)
	{
		if (name.empty() || name.size() > 255 || name.front() == '/' || name.front() == '\\' ||
			name.find('\\') != std::string_view::npos || name.find('\0') != std::string_view::npos ||
			!std::ranges::all_of(name, [](unsigned char character) { return character < 0x80; }) ||
			(name.size() >= 2 && std::isalpha(static_cast<unsigned char>(name[0])) && name[1] == ':'))
			return std::nullopt;
		std::string result;
		std::size_t start{};
		while (start <= name.size())
		{
			const auto end = name.find('/', start);
			const auto component = name.substr(start, end == std::string_view::npos ? name.size() - start : end - start);
			if (component == "..")
				return std::nullopt;
			if (!component.empty() && component != ".")
			{
				if (!std::ranges::all_of(component, [](unsigned char c) { return c >= 0x20 && c != 0x7f; }))
					return std::nullopt;
				if (!result.empty())
					result.push_back('/');
				for (const unsigned char c : component)
					result.push_back(static_cast<char>(std::tolower(c)));
			}
			if (end == std::string_view::npos)
				break;
			start = end + 1;
		}
		return result.empty() ? std::nullopt : std::optional<std::string>(std::move(result));
	}

	std::string AsciiLower(std::string_view value)
	{
		std::string result(value);
		std::ranges::transform(result, result.begin(), [](unsigned char character) {
			return static_cast<char>(std::tolower(character));
		});
		return result;
	}

	bool Identifier(std::string_view value, std::size_t maximum = 128)
	{
		return !value.empty() && value.size() <= maximum &&
			   std::ranges::all_of(value, [](unsigned char character) {
				   return std::isalnum(character) || character == '_' || character == '.' || character == '-';
			   });
	}

	bool SafeText(std::string_view value, std::size_t maximum)
	{
		return !value.empty() && value.size() <= maximum &&
			   std::ranges::all_of(value, [](unsigned char character) {
				   return character >= 0x20 || character == '\t' || character == '\n' || character == '\r';
			   });
	}

	bool HasOnlyMembers(const rapidjson::Value& object,
						std::initializer_list<std::string_view> allowed)
	{
		if (!object.IsObject())
			return false;
		for (auto member = object.MemberBegin(); member != object.MemberEnd(); ++member)
		{
			const std::string_view name(member->name.GetString(), member->name.GetStringLength());
			if (std::ranges::find(allowed, name) == allowed.end())
				return false;
		}
		return true;
	}

	std::optional<std::string> CanonicalOrigin(std::string_view value,
											 std::initializer_list<std::string_view> allowedSchemes)
	{
		if (value.empty() || value.size() > 2048 || value.find('\\') != std::string_view::npos ||
			!std::ranges::all_of(value, [](unsigned char character) {
				return character >= 0x20 && character < 0x7f;
			}))
			return std::nullopt;
		const auto separator = value.find("://");
		if (separator == std::string_view::npos)
			return std::nullopt;
		const auto scheme = value.substr(0, separator);
		if (std::ranges::find(allowedSchemes, scheme) == allowedSchemes.end())
			return std::nullopt;
		auto authority = value.substr(separator + 3);
		if (authority.empty() || authority.find_first_of("/?#@") != std::string_view::npos)
			return std::nullopt;

		std::string_view host;
		std::string_view portText;
		bool bracketed{};
		if (authority.front() == '[')
		{
			const auto close = authority.find(']');
			if (close == std::string_view::npos)
				return std::nullopt;
			host = authority.substr(1, close - 1);
			bracketed = true;
			if (close + 1 < authority.size())
			{
				if (authority[close + 1] != ':')
					return std::nullopt;
				portText = authority.substr(close + 2);
			}
		}
		else
		{
			const auto colon = authority.rfind(':');
			if (colon != std::string_view::npos)
			{
				if (authority.find(':') != colon)
					return std::nullopt;
				host = authority.substr(0, colon);
				portText = authority.substr(colon + 1);
			}
			else
			{
				host = authority;
			}
		}
		if (host.empty())
			return std::nullopt;

		std::uint32_t port = 443;
		if (!portText.empty())
		{
			const auto parsed = std::from_chars(portText.data(), portText.data() + portText.size(), port);
			if (parsed.ec != std::errc{} || parsed.ptr != portText.data() + portText.size() ||
				port == 0 || port > std::numeric_limits<std::uint16_t>::max())
				return std::nullopt;
		}
		else if (authority.ends_with(':'))
		{
			return std::nullopt;
		}

		const auto lowerHost = AsciiLower(host);
		boost::system::error_code addressError;
		const auto address = boost::asio::ip::make_address(lowerHost, addressError);
		std::string hostIdentity;
		if (!addressError)
		{
			if (address.is_v6() != bracketed)
				return std::nullopt;
			hostIdentity = address.is_v6() ? '[' + address.to_string() + ']' : address.to_string();
		}
		else
		{
			if (bracketed || lowerHost.size() > 253 || lowerHost.starts_with('.') ||
				lowerHost.ends_with('.') || lowerHost.find("..") != std::string::npos)
				return std::nullopt;
			std::size_t start{};
			while (start < lowerHost.size())
			{
				const auto end = lowerHost.find('.', start);
				const auto label = lowerHost.substr(start, end == std::string::npos ? lowerHost.size() - start : end - start);
				if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-' ||
					!std::ranges::all_of(label, [](unsigned char character) {
						return std::isalnum(character) || character == '-';
					}))
					return std::nullopt;
				if (end == std::string::npos)
					break;
				start = end + 1;
			}
			hostIdentity = lowerHost;
		}
		return fmt::format("{}://{}:{}", scheme, hostIdentity, port);
	}

	bool ParseWebUi(const rapidjson::Value& value, std::uint32_t requestedPermissions,
					CemodWebUi& output, std::string& error)
	{
		if (!HasOnlyMembers(value, {"bridge_version", "views", "network"}))
		{
			error = "web_ui contains an unknown field";
			return false;
		}
		if (!value.HasMember("bridge_version") || !value["bridge_version"].IsUint() ||
			value["bridge_version"].GetUint() != 1)
		{
			error = "web_ui.bridge_version must be 1";
			return false;
		}
		if (!value.HasMember("views") || !value["views"].IsObject() ||
			value["views"].MemberCount() == 0 || value["views"].MemberCount() > 16)
		{
			error = "web_ui.views must contain between 1 and 16 views";
			return false;
		}
		output.bridgeVersion = 1;
		for (auto member = value["views"].MemberBegin(); member != value["views"].MemberEnd(); ++member)
		{
			const std::string viewId(member->name.GetString(), member->name.GetStringLength());
			const auto& viewValue = member->value;
			if (!Identifier(viewId) || viewId.starts_with("cemu.") ||
				!HasOnlyMembers(viewValue, {"entry", "single_instance", "modes", "window", "overlay"}) ||
				!viewValue.HasMember("entry") || !viewValue["entry"].IsString())
			{
				error = "web_ui contains an invalid view";
				return false;
			}
			CemodWebUiView view;
			view.entry.assign(viewValue["entry"].GetString(), viewValue["entry"].GetStringLength());
			const auto normalizedEntry = NormalizedEntryName(view.entry);
			if (!normalizedEntry || *normalizedEntry != AsciiLower(view.entry) ||
				!view.entry.starts_with("ui/") || !AsciiLower(view.entry).ends_with(".html"))
			{
				error = fmt::format("web_ui view '{}' entry must be a safe HTML file below ui/", viewId);
				return false;
			}
			if (viewValue.HasMember("single_instance"))
			{
				if (!viewValue["single_instance"].IsBool())
				{
					error = fmt::format("web_ui view '{}' single_instance must be boolean", viewId);
					return false;
				}
				view.singleInstance = viewValue["single_instance"].GetBool();
			}
			if (!viewValue.HasMember("modes") || !viewValue["modes"].IsArray() ||
				viewValue["modes"].Empty())
			{
				error = fmt::format("web_ui view '{}' modes are invalid", viewId);
				return false;
			}
			for (const auto& mode : viewValue["modes"].GetArray())
			{
				if (!mode.IsString())
				{
					error = fmt::format("web_ui view '{}' modes are invalid", viewId);
					return false;
				}
				const std::string_view name(mode.GetString(), mode.GetStringLength());
				if (name == "window" && !view.windowMode)
					view.windowMode = true;
				else if (name == "overlay" && !view.overlayMode)
					view.overlayMode = true;
				else
				{
					error = fmt::format("web_ui view '{}' modes are invalid", viewId);
					return false;
				}
			}
			if (view.windowMode != viewValue.HasMember("window") ||
				view.overlayMode != viewValue.HasMember("overlay"))
			{
				error = fmt::format("web_ui view '{}' mode descriptors do not match modes", viewId);
				return false;
			}
			if (view.windowMode)
			{
				const auto& windowValue = viewValue["window"];
				if (!HasOnlyMembers(windowValue, {"title", "width", "height", "min_width", "min_height", "resizable"}))
				{
					error = fmt::format("web_ui view '{}' window descriptor is invalid", viewId);
					return false;
				}
				CemodWebUiWindow window;
				if (windowValue.HasMember("title"))
				{
					if (!windowValue["title"].IsString() ||
						!SafeText({windowValue["title"].GetString(), windowValue["title"].GetStringLength()}, 256))
					{
						error = fmt::format("web_ui view '{}' window title is invalid", viewId);
						return false;
					}
					window.title = std::string(windowValue["title"].GetString(), windowValue["title"].GetStringLength());
				}
				auto dimension = [&](const char* name, std::optional<std::uint32_t>& destination) {
					if (!windowValue.HasMember(name))
						return true;
					if (!windowValue[name].IsUint() || windowValue[name].GetUint() == 0 ||
						windowValue[name].GetUint() > 16384)
						return false;
					destination = windowValue[name].GetUint();
					return true;
				};
				if (!dimension("width", window.width) || !dimension("height", window.height) ||
					!dimension("min_width", window.minimumWidth) || !dimension("min_height", window.minimumHeight) ||
					(window.width && window.minimumWidth && *window.minimumWidth > *window.width) ||
					(window.height && window.minimumHeight && *window.minimumHeight > *window.height) ||
					(windowValue.HasMember("resizable") && !windowValue["resizable"].IsBool()))
				{
					error = fmt::format("web_ui view '{}' window descriptor is invalid", viewId);
					return false;
				}
				if (windowValue.HasMember("resizable"))
					window.resizable = windowValue["resizable"].GetBool();
				view.window = std::move(window);
			}
			if (view.overlayMode)
			{
				const auto& overlayValue = viewValue["overlay"];
				if (!HasOnlyMembers(overlayValue, {"surfaces", "transparent", "interactive"}) ||
					!overlayValue.HasMember("surfaces") || !overlayValue["surfaces"].IsArray() ||
					overlayValue["surfaces"].Empty())
				{
					error = fmt::format("web_ui view '{}' overlay descriptor is invalid", viewId);
					return false;
				}
				CemodWebUiOverlay overlay;
				for (const auto& surface : overlayValue["surfaces"].GetArray())
				{
					if (!surface.IsString())
					{
						error = fmt::format("web_ui view '{}' overlay surfaces are invalid", viewId);
						return false;
					}
					const std::string_view name(surface.GetString(), surface.GetStringLength());
					const auto parsed = name == "tv" ? std::optional{CemodWebUiSurface::Tv} :
						name == "drc" ? std::optional{CemodWebUiSurface::Drc} : std::nullopt;
					if (!parsed || std::ranges::find(overlay.surfaces, *parsed) != overlay.surfaces.end())
					{
						error = fmt::format("web_ui view '{}' overlay surfaces are invalid", viewId);
						return false;
					}
					overlay.surfaces.push_back(*parsed);
				}
				for (const auto* name : {"transparent", "interactive"})
					if (overlayValue.HasMember(name) && !overlayValue[name].IsBool())
					{
						error = fmt::format("web_ui view '{}' overlay {} must be boolean", viewId, name);
						return false;
					}
				if (overlayValue.HasMember("transparent"))
					overlay.transparent = overlayValue["transparent"].GetBool();
				if (overlayValue.HasMember("interactive"))
					overlay.interactive = overlayValue["interactive"].GetBool();
				view.overlay = std::move(overlay);
			}
			if (!output.views.emplace(viewId, std::move(view)).second)
			{
				error = "web_ui contains a duplicate view ID";
				return false;
			}
		}

		if (value.HasMember("network"))
		{
			const auto& network = value["network"];
			if (!HasOnlyMembers(network, {"connect", "resources", "credentials", "persistent_storage", "allow_private_network"}))
			{
				error = "web_ui.network contains an unknown field";
				return false;
			}
			auto origins = [&](const char* name, std::initializer_list<std::string_view> schemes,
							   std::vector<std::string>& destination) {
				if (!network.HasMember(name))
					return true;
				if (!network[name].IsArray() || network[name].Size() > 128)
					return false;
				for (const auto& origin : network[name].GetArray())
				{
					if (!origin.IsString())
						return false;
					const auto canonical = CanonicalOrigin(
						{origin.GetString(), origin.GetStringLength()}, schemes);
					if (!canonical || std::ranges::find(destination, *canonical) != destination.end())
						return false;
					destination.push_back(*canonical);
				}
				return true;
			};
			if (!origins("connect", {"https", "wss"}, output.network.connect) ||
				!origins("resources", {"https"}, output.network.resources))
			{
				error = "web_ui.network contains an invalid origin";
				return false;
			}
			for (const auto* name : {"credentials", "persistent_storage", "allow_private_network"})
				if (network.HasMember(name) && !network[name].IsBool())
				{
					error = fmt::format("web_ui.network.{} must be boolean", name);
					return false;
				}
			if (network.HasMember("credentials"))
				output.network.credentials = network["credentials"].GetBool();
			if (network.HasMember("persistent_storage"))
				output.network.persistentStorage = network["persistent_storage"].GetBool();
			if (network.HasMember("allow_private_network"))
				output.network.allowPrivateNetwork = network["allow_private_network"].GetBool();
		}
		const bool usesNetwork = !output.network.connect.empty() || !output.network.resources.empty() ||
			output.network.credentials || output.network.allowPrivateNetwork;
		if (usesNetwork && (requestedPermissions & 32U) == 0)
		{
			error = "web_ui network access requires the network permission";
			return false;
		}
		return true;
	}

	bool ReadEntry(zip_t* archive, zip_uint64_t index, std::uint64_t maximum,
				   std::vector<std::byte>& output)
	{
		zip_stat_t stat{};
		if (zip_stat_index(archive, index, ZIP_FL_ENC_GUESS, &stat) != 0 ||
			(stat.valid & (ZIP_STAT_SIZE | ZIP_STAT_COMP_SIZE | ZIP_STAT_COMP_METHOD)) !=
				(ZIP_STAT_SIZE | ZIP_STAT_COMP_SIZE | ZIP_STAT_COMP_METHOD) ||
			stat.size > maximum || (stat.size != 0 && stat.comp_size == 0) ||
			(stat.size > 4096 && (stat.size - 1) / stat.comp_size >= CemodPackage::kMaximumCompressionRatio) ||
			(stat.comp_method != ZIP_CM_STORE && stat.comp_method != ZIP_CM_DEFLATE) ||
			((stat.valid & ZIP_STAT_ENCRYPTION_METHOD) != 0 && stat.encryption_method != ZIP_EM_NONE))
			return false;
		std::unique_ptr<zip_file_t, ZipFileCloser> file(zip_fopen_index(archive, index, ZIP_FL_UNCHANGED));
		if (!file)
			return false;
		output.resize(static_cast<std::size_t>(stat.size));
		std::size_t offset{};
		while (offset < output.size())
		{
			const auto read = zip_fread(file.get(), output.data() + offset, output.size() - offset);
			if (read <= 0)
				return false;
			offset += static_cast<std::size_t>(read);
		}
		std::byte extra{};
		return zip_fread(file.get(), &extra, 1) == 0;
	}

	Sha256 Hash(std::span<const std::byte> bytes)
	{
		Sha256 result{};
		std::unique_ptr<EVP_MD_CTX, DigestCloser> context(EVP_MD_CTX_new());
		unsigned int size{};
		if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1 ||
			EVP_DigestUpdate(context.get(), bytes.data(), bytes.size()) != 1 ||
			EVP_DigestFinal_ex(context.get(), result.data(), &size) != 1 || size != result.size())
			result.fill(0);
		return result;
	}

	void AppendU32(std::vector<std::byte>& output, std::uint32_t value)
	{
		for (int shift = 24; shift >= 0; shift -= 8)
			output.push_back(static_cast<std::byte>((value >> shift) & 0xff));
	}

	void AppendU64(std::vector<std::byte>& output, std::uint64_t value)
	{
		for (int shift = 56; shift >= 0; shift -= 8)
			output.push_back(static_cast<std::byte>((value >> shift) & 0xff));
	}

	std::string Hex(std::span<const unsigned char> bytes)
	{
		constexpr char digits[] = "0123456789abcdef";
		std::string result(bytes.size() * 2, '0');
		for (std::size_t index = 0; index < bytes.size(); ++index)
		{
			result[index * 2] = digits[bytes[index] >> 4];
			result[index * 2 + 1] = digits[bytes[index] & 15];
		}
		return result;
	}

	bool ParseTitleId(const rapidjson::Value& value, std::uint64_t& output)
	{
		if (value.IsUint64())
		{
			output = value.GetUint64();
			return true;
		}
		if (!value.IsString() || value.GetStringLength() == 0 || value.GetStringLength() > 18)
			return false;
		std::string_view text(value.GetString(), value.GetStringLength());
		if (text.starts_with("0x") || text.starts_with("0X"))
			text.remove_prefix(2);
		const auto parsed = std::from_chars(text.data(), text.data() + text.size(), output, 16);
		return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
	}

	bool ParseManifest(std::span<const std::byte> bytes, CemodManifest& manifest, std::string& error)
	{
		if (bytes.empty() || bytes.size() > 256U * 1024U)
		{
			error = "manifest.json has an invalid size";
			return false;
		}
		rapidjson::Document document;
		document.Parse(reinterpret_cast<const char*>(bytes.data()), bytes.size());
		if (document.HasParseError() || !document.IsObject() ||
			!document.HasMember("package_version") || !document["package_version"].IsUint() ||
			(document["package_version"].GetUint() != 1 && document["package_version"].GetUint() != 2 &&
			 document["package_version"].GetUint() != 3 && document["package_version"].GetUint() != 4) ||
			!document.HasMember("api_version") ||
			!document["api_version"].IsUint() || document["api_version"].GetUint() != 2 ||
			!document.HasMember("execution_mode") || !document["execution_mode"].IsString() ||
			!document.HasMember("mod_id") || !document["mod_id"].IsString() ||
			!document.HasMember("title_ids") || !document["title_ids"].IsArray() ||
			!document.HasMember("requested_permissions") || !document["requested_permissions"].IsArray())
		{
			error = "manifest.json does not match the CEX2 schema";
			return false;
		}
		manifest.packageVersion = document["package_version"].GetUint();
		manifest.apiVersion = document["api_version"].GetUint();
		if (manifest.packageVersion == 1)
		{
			if (document.HasMember("payload") || document.HasMember("scope") || document.HasMember("permissions"))
			{
				error = "package_version 1 must not contain version 2 payload, scope, or permissions";
				return false;
			}
			manifest.payload = {CemodPayloadFormat::CemodElf, "mod.elf"};
		}
		else
		{
			if (!document.HasMember("payload") || !document["payload"].IsObject())
			{
				error = "package_version 2, 3 or 4 requires a payload descriptor";
				return false;
			}
			const auto& payload = document["payload"];
			if (!payload.HasMember("format") || !payload["format"].IsString() ||
				!payload.HasMember("path") || !payload["path"].IsString() || payload.MemberCount() != 2)
			{
				error = "payload descriptor must contain only string format and path fields";
				return false;
			}
			const std::string_view format(payload["format"].GetString(), payload["format"].GetStringLength());
			const std::string_view payloadPath(payload["path"].GetString(), payload["path"].GetStringLength());
			if (format == "cemod_elf")
				manifest.payload = {CemodPayloadFormat::CemodElf, "mod.elf"};
			else if (format == "wups")
				manifest.payload = {CemodPayloadFormat::Wups, "plugin.wps"};
			else
			{
				error = fmt::format("unknown payload format '{}'", format);
				return false;
			}
			if (payloadPath != manifest.payload.path)
			{
				error = fmt::format("payload path '{}' does not match format '{}'", payloadPath, format);
				return false;
			}
		}
		const std::string_view executionMode(document["execution_mode"].GetString(),
											 document["execution_mode"].GetStringLength());
		if (executionMode == "isolated")
			manifest.executionMode = CemodExecutionMode::Isolated;
		else if (executionMode == "trusted_native")
			manifest.executionMode = CemodExecutionMode::TrustedNative;
		else
		{
			error = "execution_mode must be isolated or trusted_native";
			return false;
		}
		if (manifest.payload.format == CemodPayloadFormat::Wups &&
			manifest.executionMode != CemodExecutionMode::TrustedNative)
		{
			error = "WUPS payloads require execution_mode trusted_native";
			return false;
		}
		if (document.HasMember("lifecycle"))
		{
			if (manifest.packageVersion < 3 || !document["lifecycle"].IsObject())
			{
				error = "lifecycle requires package_version 3";
				return false;
			}
			const auto& lifecycle = document["lifecycle"];
			if (lifecycle.MemberCount() != 1 || !lifecycle.HasMember("unload") ||
				!lifecycle["unload"].IsString())
			{
				error = "lifecycle must contain only a string unload field";
				return false;
			}
			const std::string_view unload(lifecycle["unload"].GetString(),
										  lifecycle["unload"].GetStringLength());
			if (unload != "after_title_threads_stop")
			{
				error = fmt::format("unknown lifecycle unload policy '{}'", unload);
				return false;
			}
			if (manifest.payload.format != CemodPayloadFormat::Wups ||
				manifest.executionMode != CemodExecutionMode::TrustedNative)
			{
				error = "after_title_threads_stop is valid only for trusted_native WUPS payloads";
				return false;
			}
			manifest.unloadPolicy = CemodUnloadPolicy::AfterTitleThreadsStop;
		}
		manifest.modId.assign(document["mod_id"].GetString(), document["mod_id"].GetStringLength());
		if (manifest.modId.empty() || manifest.modId.size() > 128 ||
			!std::ranges::all_of(manifest.modId, [](unsigned char c) {
				return std::isalnum(c) || c == '.' || c == '_' || c == '-';
			}))
		{
			error = "mod_id is invalid";
			return false;
		}
		if (document["title_ids"].Empty() || document["title_ids"].Size() > 64)
		{
			error = "title_ids is empty or too large";
			return false;
		}
		for (const auto& value : document["title_ids"].GetArray())
		{
			std::uint64_t title{};
			if (!ParseTitleId(value, title) || title == 0)
			{
				error = "title_ids contains an invalid title ID";
				return false;
			}
			manifest.titleIds.push_back(title);
		}
		static const std::map<std::string_view, std::uint32_t> permissionBits{
			{"read", 1U},
			{"write", 2U},
			{"inject", 4U},
			{"clipboard", 8U},
			{"capture", 16U},
			{"network", 32U},
			{"ui", 64U},
		};
		for (const auto& value : document["requested_permissions"].GetArray())
		{
			if (!value.IsString())
			{
				error = "requested_permissions contains a non-string value";
				return false;
			}
			const std::string_view name(value.GetString(), value.GetStringLength());
			const auto found = permissionBits.find(name);
			if (found == permissionBits.end() || (manifest.requestedPermissions & found->second) != 0)
			{
				error = "requested_permissions contains an unknown or duplicate value";
				return false;
			}
			manifest.requestedPermissions |= found->second;
		}
		if (manifest.packageVersion < 4 &&
			((manifest.requestedPermissions & 64U) != 0 || document.HasMember("web_ui")))
		{
			error = "ui permission and web_ui require package_version 4";
			return false;
		}
		if (manifest.packageVersion == 4)
		{
			if ((manifest.requestedPermissions & 64U) == 0 || !document.HasMember("web_ui"))
			{
				error = "package_version 4 requires ui permission and web_ui";
				return false;
			}
			CemodWebUi webUi;
			if (!ParseWebUi(document["web_ui"], manifest.requestedPermissions, webUi, error))
				return false;
			manifest.webUi = std::move(webUi);
		}
		if (manifest.packageVersion >= 2 && document.HasMember("scope"))
		{
			if (!document["scope"].IsObject() || !document["scope"].HasMember("type") ||
				!document["scope"]["type"].IsString())
			{
				error = "scope must be an object with a string type";
				return false;
			}
			const auto& scope = document["scope"];
			const std::string_view type(scope["type"].GetString(), scope["type"].GetStringLength());
			if (type == "aroma_native")
			{
				if (scope.MemberCount() != 1)
				{
					error = "aroma_native scope must not contain targets";
					return false;
				}
				manifest.scope.type = CemodScopeType::AromaNative;
			}
			else if (type == "process")
			{
				if (scope.MemberCount() != 2 || !scope.HasMember("targets") || !scope["targets"].IsArray() ||
					scope["targets"].Empty() || scope["targets"].Size() > 16)
				{
					error = "process scope requires a non-empty targets array";
					return false;
				}
				static const std::set<std::string_view> allowedTargets{
					"all", "root_rpx", "wii_u_menu", "tvii", "e_manual", "home_menu", "error_display",
					"mini_miiverse", "browser", "miiverse", "eshop", "download_manager", "game", "game_and_menu"};
				std::set<std::string> targets;
				for (const auto& value : scope["targets"].GetArray())
				{
					if (!value.IsString())
					{
						error = "process scope contains a non-string target";
						return false;
					}
					std::string target(value.GetString(), value.GetStringLength());
					if (!allowedTargets.contains(target) || !targets.insert(target).second)
					{
						error = "process scope contains an unknown or duplicate target";
						return false;
					}
				}
				manifest.scope.type = CemodScopeType::Process;
				manifest.scope.targets.assign(targets.begin(), targets.end());
			}
			else
			{
				error = fmt::format("unknown scope type '{}'", type);
				return false;
			}
		}
		if (manifest.packageVersion >= 2 && document.HasMember("permissions"))
		{
			if (!document["permissions"].IsObject())
			{
				error = "permissions must be an object";
				return false;
			}
			const auto& permissions = document["permissions"];
			if (manifest.packageVersion < 3 &&
				permissions.HasMember("plugin_management"))
			{
				error = "plugin_management requires package_version 3";
				return false;
			}
			static const std::set<std::string_view> allowedPermissionFields{
				"native_memory", "function_patching", "physical_address_patching", "filesystem", "network",
				"mapped_memory", "notifications", "content_redirection", "modules",
				"plugin_management"};
			for (auto member = permissions.MemberBegin(); member != permissions.MemberEnd(); ++member)
				if (!allowedPermissionFields.contains(std::string_view(member->name.GetString(), member->name.GetStringLength())))
				{
					error = fmt::format("permissions contains unknown field '{}'", member->name.GetString());
					return false;
				}
			auto boolean = [&](const char* name, bool& output) {
				if (!permissions.HasMember(name))
					return true;
				if (!permissions[name].IsBool())
					return false;
				output = permissions[name].GetBool();
				return true;
			};
			if (!boolean("native_memory", manifest.nativePermissions.nativeMemory) ||
				!boolean("function_patching", manifest.nativePermissions.functionPatching) ||
				!boolean("physical_address_patching", manifest.nativePermissions.physicalAddressPatching) ||
				!boolean("network", manifest.nativePermissions.network) ||
				!boolean("mapped_memory", manifest.nativePermissions.mappedMemory) ||
				!boolean("notifications", manifest.nativePermissions.notifications) ||
				!boolean("content_redirection", manifest.nativePermissions.contentRedirection) ||
				!boolean("plugin_management", manifest.nativePermissions.pluginManagement))
			{
				error = "permissions boolean field has the wrong type";
				return false;
			}
			if (permissions.HasMember("filesystem"))
			{
				const auto& filesystem = permissions["filesystem"];
				if (!filesystem.IsObject() || filesystem.MemberCount() > 2 ||
					(filesystem.HasMember("read") && !filesystem["read"].IsBool()) ||
					(filesystem.HasMember("write") && !filesystem["write"].IsBool()))
				{
					error = "permissions.filesystem must contain only boolean read/write fields";
					return false;
				}
				for (auto member = filesystem.MemberBegin(); member != filesystem.MemberEnd(); ++member)
					if (std::string_view(member->name.GetString(), member->name.GetStringLength()) != "read" &&
						std::string_view(member->name.GetString(), member->name.GetStringLength()) != "write")
					{
						error = "permissions.filesystem contains an unknown field";
						return false;
					}
				if (filesystem.HasMember("read"))
					manifest.nativePermissions.filesystemRead = filesystem["read"].GetBool();
				if (filesystem.HasMember("write"))
					manifest.nativePermissions.filesystemWrite = filesystem["write"].GetBool();
			}
			if (permissions.HasMember("modules"))
			{
				if (!permissions["modules"].IsArray() || permissions["modules"].Size() > 64)
				{
					error = "permissions.modules must be an array with at most 64 entries";
					return false;
				}
				std::set<std::string> modules;
				for (const auto& value : permissions["modules"].GetArray())
				{
					if (!value.IsString())
					{
						error = "permissions.modules contains a non-string value";
						return false;
					}
					std::string module(value.GetString(), value.GetStringLength());
					if (module.empty() || module.size() > 128 ||
						!std::ranges::all_of(module, [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '.' || c == '-'; }) ||
						!modules.insert(module).second)
					{
						error = "permissions.modules contains an invalid or duplicate module";
						return false;
					}
				}
				manifest.nativePermissions.modules.assign(modules.begin(), modules.end());
			}
		}
		if (manifest.executionMode == CemodExecutionMode::TrustedNative)
		{
			if (document.HasMember("cpu") || document.HasMember("entrypoint"))
			{
				error = "trusted_native manifests must not contain isolated resource or lifecycle fields";
				return false;
			}
			if (document.HasMember("memory"))
			{
				if (manifest.packageVersion < 3 || !document["memory"].IsObject())
				{
					error = "trusted_native memory requests require package_version 3";
					return false;
				}
				const auto& memory = document["memory"];
				if (memory.MemberCount() != 1 || !memory.HasMember("mem2_expansion_bytes") ||
					!memory["mem2_expansion_bytes"].IsUint())
				{
					error = "trusted_native memory must contain only mem2_expansion_bytes";
					return false;
				}
				manifest.mem2ExpansionBytes = memory["mem2_expansion_bytes"].GetUint();
				constexpr std::uint32_t maximumExpansion = 256U * 1024U * 1024U;
				if (manifest.mem2ExpansionBytes == 0 ||
					manifest.mem2ExpansionBytes > maximumExpansion ||
					(manifest.mem2ExpansionBytes & 0xFFFU) != 0)
				{
					error = "mem2_expansion_bytes must be page-aligned and at most 256 MiB";
					return false;
				}
			}
			manifest.codeBytes = 8U * 1024U * 1024U;
			return true;
		}
		if (!document.HasMember("memory") || !document["memory"].IsObject() ||
			!document.HasMember("cpu") || !document["cpu"].IsObject() ||
			!document.HasMember("entrypoint") || !document["entrypoint"].IsString())
		{
			error = "isolated manifests require memory, cpu, and entrypoint";
			return false;
		}
		const auto& memory = document["memory"];
		const auto& cpu = document["cpu"];
		if (!memory.HasMember("code_bytes") || !memory["code_bytes"].IsUint() ||
			!memory.HasMember("private_bytes") || !memory["private_bytes"].IsUint() ||
			!memory.HasMember("stack_bytes") || !memory["stack_bytes"].IsUint() ||
			!cpu.HasMember("instructions_per_frame") || !cpu["instructions_per_frame"].IsUint() ||
			!cpu.HasMember("time_us_per_frame") || !cpu["time_us_per_frame"].IsUint())
		{
			error = "memory or cpu limits are missing";
			return false;
		}
		manifest.codeBytes = memory["code_bytes"].GetUint();
		manifest.privateBytes = memory["private_bytes"].GetUint();
		manifest.stackBytes = memory["stack_bytes"].GetUint();
		manifest.instructionsPerFrame = cpu["instructions_per_frame"].GetUint();
		manifest.timeMicrosecondsPerFrame = cpu["time_us_per_frame"].GetUint();
		manifest.entrypoint.assign(document["entrypoint"].GetString(), document["entrypoint"].GetStringLength());
		if (manifest.codeBytes == 0 || manifest.codeBytes > 16U * 1024U * 1024U ||
			manifest.privateBytes == 0 || manifest.privateBytes > 32U * 1024U * 1024U ||
			manifest.stackBytes == 0 || manifest.stackBytes > 1U * 1024U * 1024U ||
			manifest.stackBytes % 4096U != 0 ||
			manifest.instructionsPerFrame == 0 ||
			manifest.instructionsPerFrame > 1'000'000U ||
			manifest.timeMicrosecondsPerFrame == 0 || manifest.timeMicrosecondsPerFrame > 1000 ||
			manifest.entrypoint != "cemod_init")
		{
			error = "manifest resource limits or entrypoint are invalid";
			return false;
		}
		return true;
	}

	bool ValidateElf(std::span<const std::byte> elf, const CemodManifest& manifest, std::string& error)
	{
		const auto maximumElfBytes = manifest.executionMode == CemodExecutionMode::TrustedNative ? CemodPackage::kMaximumExpandedBytes : static_cast<std::uint64_t>(manifest.codeBytes) + manifest.privateBytes;
		if (elf.size() < 52 || elf.size() > maximumElfBytes)
		{
			error = "PPC ELF has an invalid size";
			return false;
		}
		const auto* data = reinterpret_cast<const unsigned char*>(elf.data());
		auto u16 = [data](std::size_t offset) {
			return static_cast<std::uint16_t>((data[offset] << 8) | data[offset + 1]);
		};
		auto u32 = [data](std::size_t offset) {
			return (static_cast<std::uint32_t>(data[offset]) << 24) |
				   (static_cast<std::uint32_t>(data[offset + 1]) << 16) |
				   (static_cast<std::uint32_t>(data[offset + 2]) << 8) | data[offset + 3];
		};
		if (data[0] != 0x7f || data[1] != 'E' || data[2] != 'L' || data[3] != 'F' ||
			data[4] != 1 || data[5] != 2 || data[6] != 1 || u16(18) != 20 || u32(20) != 1 ||
			(manifest.executionMode == CemodExecutionMode::TrustedNative && u16(16) != 3))
		{
			error = "package executable is not a 32-bit big-endian PPC ELF";
			return false;
		}
		const auto programOffset = u32(28);
		const auto programEntrySize = u16(42);
		const auto programCount = u16(44);
		if (programCount == 0 || programCount > 128 || programEntrySize < 32 ||
			programOffset > elf.size() || static_cast<std::uint64_t>(programCount) * programEntrySize > elf.size() - programOffset)
		{
			error = "PPC ELF program table is out of bounds";
			return false;
		}
		std::uint64_t codeBytes{};
		std::uint64_t dataBytes{};
		struct LoadRange
		{
			std::uint32_t address;
			std::uint32_t size;
			std::uint32_t flags;
		};
		std::vector<LoadRange> loadRanges;
		for (std::uint16_t index = 0; index < programCount; ++index)
		{
			const auto offset = programOffset + static_cast<std::uint32_t>(index) * programEntrySize;
			if (u32(offset) != 1)
				continue;
			const auto fileOffset = u32(offset + 4);
			const auto fileSize = u32(offset + 16);
			const auto memorySize = u32(offset + 20);
			const auto flags = u32(offset + 24);
			if (fileSize > memorySize || fileOffset > elf.size() || fileSize > elf.size() - fileOffset ||
				((flags & 2U) != 0 && (flags & 1U) != 0))
			{
				error = "PPC ELF contains an invalid or writable-executable segment";
				return false;
			}
			if ((flags & 1U) != 0)
				codeBytes += memorySize;
			else
				dataBytes += memorySize;
			loadRanges.push_back({u32(offset + 8), memorySize, flags});
		}
		if (codeBytes == 0 ||
			(manifest.executionMode == CemodExecutionMode::Isolated &&
			 (codeBytes > manifest.codeBytes || dataBytes > manifest.privateBytes)) ||
			(manifest.executionMode == CemodExecutionMode::TrustedNative &&
			 codeBytes + dataBytes > 8U * 1024U * 1024U))
		{
			error = "PPC ELF exceeds the manifest memory limits";
			return false;
		}
		if (manifest.executionMode == CemodExecutionMode::TrustedNative)
		{
			struct Section
			{
				std::uint32_t name{}, type{}, flags{}, address{}, offset{}, size{}, link{}, entrySize{};
			};
			const auto sectionOffset = u32(32);
			const auto sectionEntrySize = u16(46);
			const auto sectionCount = u16(48);
			const auto sectionNames = u16(50);
			if (sectionEntrySize < 40 || sectionCount == 0 || sectionCount > 1024 ||
				sectionNames >= sectionCount || sectionOffset > elf.size() ||
				static_cast<std::uint64_t>(sectionCount) * sectionEntrySize > elf.size() - sectionOffset)
			{
				error = "trusted ELF section table is invalid";
				return false;
			}
			std::vector<Section> sections;
			sections.reserve(sectionCount);
			for (std::uint16_t index = 0; index < sectionCount; ++index)
			{
				const auto offset = sectionOffset + static_cast<std::uint32_t>(index) * sectionEntrySize;
				Section section{u32(offset), u32(offset + 4), u32(offset + 8), u32(offset + 12),
								u32(offset + 16), u32(offset + 20), u32(offset + 24), u32(offset + 36)};
				if (section.type != 8 && (section.offset > elf.size() || section.size > elf.size() - section.offset))
				{
					error = "trusted ELF section is out of bounds";
					return false;
				}
				sections.push_back(section);
			}
			const auto& names = sections[sectionNames];
			if (names.type != 3 || names.offset > elf.size() || names.size > elf.size() - names.offset)
			{
				error = "trusted ELF section-name table is invalid";
				return false;
			}
			auto sectionName = [&](const Section& section) -> std::optional<std::string_view> {
				if (section.name >= names.size)
					return std::nullopt;
				const auto* value = reinterpret_cast<const char*>(elf.data() + names.offset + section.name);
				const auto maximum = names.size - section.name;
				const auto length = strnlen(value, maximum);
				if (length == maximum)
					return std::nullopt;
				return std::string_view(value, length);
			};
			auto imageContains = [&](std::uint32_t address, std::uint32_t size, bool executable) {
				return std::ranges::any_of(loadRanges, [&](const LoadRange& range) {
					return (!executable || (range.flags & 1U) != 0) && address >= range.address &&
						   address - range.address <= range.size && size <= range.size - (address - range.address);
				});
			};
			std::optional<std::size_t> bootstrap;
			static constexpr std::array<std::uint8_t, 8> allowedRelocations{0, 1, 4, 5, 6, 10, 22, 26};
			for (std::size_t index = 0; index < sections.size(); ++index)
			{
				const auto& section = sections[index];
				const auto name = sectionName(section);
				if (!name)
				{
					error = "trusted ELF contains an invalid section name";
					return false;
				}
				if (*name == ".cemod.bootstrap")
				{
					if (bootstrap)
					{
						error = "trusted ELF contains duplicate bootstrap sections";
						return false;
					}
					bootstrap = index;
				}
				if (section.type == 9)
				{
					error = "trusted ELF must use RELA relocations";
					return false;
				}
				if (section.type == 4)
				{
					if (section.entrySize < 12 || section.size % section.entrySize != 0 ||
						section.link >= sections.size() ||
						(sections[section.link].type != 2 && sections[section.link].type != 11))
					{
						error = "trusted ELF relocation table is invalid";
						return false;
					}
					const auto& symbols = sections[section.link];
					if (symbols.entrySize < 16 || symbols.size % symbols.entrySize != 0)
					{
						error = "trusted ELF relocation symbol table is invalid";
						return false;
					}
					for (std::uint32_t entry = 0; entry < section.size / section.entrySize; ++entry)
					{
						const auto relocation = section.offset + entry * section.entrySize;
						const auto info = u32(relocation + 4);
						const auto type = static_cast<std::uint8_t>(info);
						if (std::ranges::find(allowedRelocations, type) == allowedRelocations.end() ||
							(info >> 8) >= symbols.size / symbols.entrySize)
						{
							error = "trusted ELF contains an unsupported relocation";
							return false;
						}
						const auto width = (type == 4 || type == 5 || type == 6) ? 2U : 4U;
						if (type != 0 && !imageContains(u32(relocation), width, false))
						{
							error = "trusted ELF relocation target is outside the image";
							return false;
						}
					}
				}
				if (section.type == 2 || section.type == 11)
				{
					if (section.entrySize < 16 || section.size % section.entrySize != 0)
					{
						error = "trusted ELF symbol table is invalid";
						return false;
					}
					for (std::uint32_t symbol = 1; symbol < section.size / section.entrySize; ++symbol)
						if (u16(section.offset + symbol * section.entrySize + 14) == 0)
						{
							error = "trusted ELF contains an undefined symbol";
							return false;
						}
				}
			}
			if (!bootstrap)
			{
				error = "trusted ELF is missing .cemod.bootstrap";
				return false;
			}
			const auto& table = sections[*bootstrap];
			if (table.type != 1 || (table.flags & 2U) == 0 || table.size < 12 ||
				!imageContains(table.address, table.size, false) || u32(table.offset) != 0x434d4231U ||
				u16(table.offset + 4) != 1 || u16(table.offset + 6) != 24)
			{
				error = "trusted ELF contains an invalid CMB1 bootstrap section";
				return false;
			}
			const auto recordCount = u32(table.offset + 8);
			if (recordCount == 0 || recordCount > 64 || table.size != 12U + recordCount * 24U)
			{
				error = "trusted ELF contains an invalid CMB1 record count";
				return false;
			}
			for (std::uint32_t record = 0; record < recordCount; ++record)
			{
				const auto offset = table.offset + 12 + record * 24;
				if (u32(offset) == 0 || (u32(offset + 4) & 3U) != 0 || u32(offset + 12) == 0 ||
					u32(offset + 20) != 0 || !imageContains(u32(offset + 16), 4, true))
				{
					error = "trusted ELF contains an invalid CMB1 record";
					return false;
				}
			}
		}
		return true;
	}

} // namespace

std::optional<CemodPackage> CemodPackage::Inspect(const std::filesystem::path& path,
												  std::string& error)
{
	error.clear();
	if (path.extension() != ".cemod")
	{
		error = "package must use the .cemod extension";
		return std::nullopt;
	}
	std::error_code filesystemError;
	const auto packageSize = std::filesystem::file_size(path, filesystemError);
	if (filesystemError || packageSize == 0 || packageSize > kMaximumArchiveBytes)
	{
		error = "package file has an invalid size";
		return std::nullopt;
	}
	int zipError{};
	std::unique_ptr<zip_t, ZipCloser> archive(zip_open(path.string().c_str(), ZIP_RDONLY, &zipError));
	if (!archive)
	{
		error = "package is not a readable ZIP container";
		return std::nullopt;
	}
	const auto count = zip_get_num_entries(archive.get(), 0);
	if (count <= 0 || static_cast<std::uint64_t>(count) > kMaximumPackageEntries)
	{
		error = "package contains an invalid number of entries";
		return std::nullopt;
	}
	std::map<std::string, std::vector<std::byte>> entries;
	std::set<std::string> normalizedNames;
	std::uint64_t expandedBytes{};
	std::size_t uiFiles{};
	std::uint64_t uiBytes{};
	static const std::set<std::string_view> allowedEntries{
		"manifest.json", "mod.elf", "plugin.wps", "public_key.ed25519", "signature.ed25519"};
	for (zip_int64_t index = 0; index < count; ++index)
	{
		const char* rawName = zip_get_name(archive.get(), index, ZIP_FL_ENC_STRICT);
		const auto normalized = rawName ? NormalizedEntryName(rawName) : std::nullopt;
		if (!rawName || !normalized || std::string_view(rawName).ends_with('/'))
		{
			error = "package contains an unsafe entry name";
			return std::nullopt;
		}
		if (!normalizedNames.insert(*normalized).second)
		{
			error = "package contains a duplicate normalized entry name";
			return std::nullopt;
		}
		const std::string_view entryName(rawName);
		const bool uiEntry = entryName.starts_with("ui/");
		if ((!allowedEntries.contains(entryName) && !uiEntry) ||
			(uiEntry && *normalized != AsciiLower(entryName)))
		{
			error = fmt::format("package contains unknown mandatory entry '{}'", rawName);
			return std::nullopt;
		}
		zip_uint8_t operatingSystem{};
		zip_uint32_t attributes{};
		if (zip_file_get_external_attributes(archive.get(), index, ZIP_FL_UNCHANGED,
										 &operatingSystem, &attributes) != 0)
		{
			error = "package entry attributes cannot be read";
			return std::nullopt;
		}
		const auto fileType = (attributes >> 16U) & 0170000U;
		if (fileType != 0 && fileType != 0100000U)
		{
			error = "package contains a non-regular entry";
			return std::nullopt;
		}
		if (uiEntry && ++uiFiles > kMaximumUiFiles)
		{
			error = "package UI files exceed the count limit";
			return std::nullopt;
		}
		std::vector<std::byte> data;
		const auto maximumEntry = uiEntry ?
			std::min({kMaximumExpandedBytes - expandedBytes, kMaximumUiFileBytes,
					  kMaximumUiBytes - uiBytes}) :
			kMaximumExpandedBytes - expandedBytes;
		if (!ReadEntry(archive.get(), index, maximumEntry, data))
		{
			error = "package entry cannot be read or exceeds the expansion limit";
			return std::nullopt;
		}
		expandedBytes += data.size();
		if (uiEntry)
			uiBytes += data.size();
		if (!entries.emplace(rawName, std::move(data)).second)
		{
			error = "package contains a duplicate entry name";
			return std::nullopt;
		}
	}
	const auto manifestEntry = entries.find("manifest.json");
	if (manifestEntry == entries.end())
	{
		error = "package must contain manifest.json";
		return std::nullopt;
	}
	CemodPackage result;
	if (!ParseManifest(manifestEntry->second, result.manifest, error))
	{
		return std::nullopt;
	}
	const auto hasUiEntries = std::ranges::any_of(entries, [](const auto& entry) {
		return entry.first.starts_with("ui/");
	});
	if (result.manifest.packageVersion != 4 && hasUiEntries)
	{
		error = "UI files require package_version 4";
		return std::nullopt;
	}
	if (result.manifest.packageVersion == 4)
	{
		if (!hasUiEntries)
		{
			error = "package_version 4 package contains no UI files";
			return std::nullopt;
		}
		for (const auto& [viewId, view] : result.manifest.webUi->views)
		{
			if (!entries.contains(view.entry))
			{
				error = fmt::format("web_ui view '{}' entry is missing from the package", viewId);
				return std::nullopt;
			}
		}
	}
	const auto elfEntry = entries.find("mod.elf");
	const auto wpsEntry = entries.find("plugin.wps");
	if ((elfEntry == entries.end()) == (wpsEntry == entries.end()))
	{
		error = "package must contain exactly one of mod.elf or plugin.wps";
		return std::nullopt;
	}
	const auto expected = entries.find(result.manifest.payload.path);
	if (expected == entries.end() ||
		(result.manifest.payload.format == CemodPayloadFormat::CemodElf && wpsEntry != entries.end()) ||
		(result.manifest.payload.format == CemodPayloadFormat::Wups && elfEntry != entries.end()))
	{
		error = "manifest payload descriptor does not match the packaged payload";
		return std::nullopt;
	}
	if (expected->second.empty() || expected->second.size() > kMaximumPayloadBytes)
	{
		error = "package payload has an invalid size";
		return std::nullopt;
	}
	if (result.manifest.payload.format == CemodPayloadFormat::CemodElf)
	{
		if (!ValidateElf(expected->second, result.manifest, error))
			return std::nullopt;
	}
	else
	{
		result.wups = WupsBinaryInspector::Inspect(expected->second, error);
		if (!result.wups)
			return std::nullopt;
	}
	result.payload = expected->second;
	if (result.manifest.payload.format == CemodPayloadFormat::CemodElf)
		result.elf = result.payload; // Preserve the public legacy field for existing callers.

	const auto signature = entries.find("signature.ed25519");
	const auto publicKey = entries.find("public_key.ed25519");
	if ((signature == entries.end()) != (publicKey == entries.end()))
	{
		error = "signature and public key must either both be present or both be absent";
		return std::nullopt;
	}
	if (signature != entries.end())
	{
		if (signature->second.size() != 64 || publicKey->second.size() != 32)
		{
			error = "Ed25519 signature material has an invalid size";
			return std::nullopt;
		}
		std::vector<std::byte> canonical;
		for (const auto& [name, data] : entries)
		{
			if (name == "signature.ed25519")
				continue;
			AppendU32(canonical, static_cast<std::uint32_t>(name.size()));
			canonical.insert(canonical.end(), reinterpret_cast<const std::byte*>(name.data()),
							 reinterpret_cast<const std::byte*>(name.data() + name.size()));
			AppendU64(canonical, data.size());
			const auto digest = Hash(data);
			canonical.insert(canonical.end(), reinterpret_cast<const std::byte*>(digest.data()),
							 reinterpret_cast<const std::byte*>(digest.data() + digest.size()));
		}
		const auto digest = Hash(canonical);
		std::unique_ptr<EVP_PKEY, KeyCloser> key(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
																			 reinterpret_cast<const unsigned char*>(publicKey->second.data()), publicKey->second.size()));
		std::unique_ptr<EVP_MD_CTX, DigestCloser> verify(EVP_MD_CTX_new());
		if (!key || !verify || EVP_DigestVerifyInit(verify.get(), nullptr, nullptr, nullptr, key.get()) != 1 ||
			EVP_DigestVerify(verify.get(), reinterpret_cast<const unsigned char*>(signature->second.data()),
							 signature->second.size(), digest.data(), digest.size()) != 1)
		{
			error = "Ed25519 signature verification failed";
			return std::nullopt;
		}
		const auto fingerprint = Hash(publicKey->second);
		result.principal = "ed25519:" + Hex(fingerprint) + ":" + result.manifest.modId;
		result.signedPackage = true;
	}
	else
	{
		std::ifstream file(path, std::ios::binary);
		std::vector<std::byte> packageBytes(static_cast<std::size_t>(packageSize));
		if (!file.read(reinterpret_cast<char*>(packageBytes.data()), packageBytes.size()))
		{
			error = "package cannot be hashed";
			return std::nullopt;
		}
		const auto digest = Hash(packageBytes);
		result.principal = "sha256:" + Hex(digest);
	}
	for (auto& [name, data] : entries)
		if (name.starts_with("ui/"))
			result.uiAssets.emplace(name, std::move(data));
	return result;
}

std::optional<CemodPackage> CemodPackage::Load(const std::filesystem::path& path,
											   std::uint64_t titleId, std::string& error)
{
	auto result = Inspect(path, error);
	if (!result)
		return std::nullopt;
	if (titleId == 0 || std::ranges::find(result->manifest.titleIds, titleId) == result->manifest.titleIds.end())
	{
		error = "package does not target the active title";
		return std::nullopt;
	}
	result->targetTitleId = titleId;
	return result;
}
