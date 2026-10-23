#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit_FS.h"
#include "Cafe/OS/libs/gx2/GX2.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/OS/libs/swkbd/swkbd.h"
#include "frontend/RuntimeOverlay.h"
#include "host/contracts/HostContracts.h"

#include <mutex>

#if defined(CEMU_OVERLAY_BACKEND_IMGUI)
#include <imgui.h>
#include "imgui/imgui_extension.h"
#include "util/helpers/helpers.h"
#include "resource/IconsFontAwesome5.h"
#endif

#define SWKBD_FORM_STRING_MAX_LENGTH (4096) // counted in 16-bit characters

#define SWKBD_STATE_BLANK (0)		 // not visible
#define SWKBD_STATE_APPEARING (1)	 // fade-in ?
#define SWKBD_STATE_DISPLAYED (2)	 // visible
#define SWKBD_STATE_DISAPPEARING (3) // fade-out ?

typedef struct
{
	uint32 ukn00;			   // constructor?
	uint32 ukn04;			   // destructor?
	uint32 ukn08;			   // ?
	MEMPTR<void> changeString; // some function address
} SwkbdIEventReceiverVTable_t;

typedef struct
{
	MEMPTR<SwkbdIEventReceiverVTable_t> vTable;
	// todo - more elements? (currently separated from this struct)
} SwkbdIEventReceiver_t;

struct swkbdReceiverArg_t
{
	MEMPTR<SwkbdIEventReceiver_t> IEventReceiver;
	MEMPTR<sint16be> stringBuf;
	sint32be stringBufSize;
	sint32be fixedCharLimit;
	sint32be cursorPos;
	sint32be selectFrom;
};

typedef struct
{
	uint32 ukn000;
	uint32 controllerType;
	uint32 keyboardMode; // guessed
	uint32 ukn00C;
	uint32 ukn010;
	uint32 ukn014;
	uint32 ukn018;
	uint32 ukn01C; // ok string?
	uint32 ukn020[4];
	uint32 ukn030[4];
	uint32 ukn040[4];
	uint32 ukn050[4];
	uint32 ukn060[4];
	uint32 ukn070[4];
	uint32 ukn080[4];
	uint32 ukn090[4];
	uint32 ukn0A0;
	uint32 ukn0A4;
	// uint32 ukn0A8;
	// MEMPTR<SwkbdIEventReceiver_t> IEventReceiver;
	swkbdReceiverArg_t receiverArg;
} SwkbdKeyboardArg_t;

typedef struct
{
	// this structure resides in PPC addressable memory space
	wchar_t formStringBuffer[SWKBD_FORM_STRING_MAX_LENGTH];
	sint32 formStringLength;
	// big endian version of the string buffer (converted whenever GetInputFormString is called)
	uint16be formStringBufferBE[SWKBD_FORM_STRING_MAX_LENGTH];
	bool isActive; // set when SwkbdAppearInputForm() is called
	// bool isDisplayed; // set when keyboard is rendering
	bool decideButtonWasPressed; // set to false when keyboard appears, and set to true when enter is pressed. Remains on true after the keyboard is disappeared (todo: Investigate how this really works)
	// keyboard only mode (no input form)
	bool keyboardOnlyMode;
	SwkbdKeyboardArg_t keyboardArg;
	// input form appear args
	sint32 maxTextLength;
	// imgui keyboard drawing stuff
	bool shiftActivated;
	bool returnState;
	bool cancelState;
} swkbdInternalState_t;

swkbdInternalState_t* swkbdInternalState = nullptr;
std::uint64_t swkbdOverlayGeneration = 0;
sint32 isNeedCalcSubThreadFont = 0;
sint32 isNeedCalcSubThreadPredict = 0;

namespace
{
	void PublishSoftwareKeyboard()
	{
		RuntimeOverlay::SoftwareKeyboard keyboard{.generation = swkbdOverlayGeneration};
		if (swkbdInternalState)
		{
			keyboard.active = swkbdInternalState->isActive;
			keyboard.keyboardOnly = swkbdInternalState->keyboardOnlyMode;
			keyboard.shifted = swkbdInternalState->shiftActivated;
			keyboard.maximumLength = static_cast<std::uint32_t>(
				std::max<sint32>(0, swkbdInternalState->maxTextLength));
			keyboard.text = boost::nowide::narrow(
				fmt::format(L"{}", swkbdInternalState->formStringBuffer));
		}
		RuntimeOverlay::Model::Instance().SetSoftwareKeyboard(std::move(keyboard));
	}
} // namespace

void swkbd_inputStringChanged();

namespace
{
	// Host-side editing state of the input form. The text itself stays in
	// swkbdInternalState->formStringBuffer; this tracks caret, selection and undo.
	enum class SwkbdEditGroup
	{
		None,
		Typing,
		Erasing,
	};

	struct SwkbdUndoEntry
	{
		std::wstring text;
		sint32 cursor;
		sint32 anchor;
	};

	struct SwkbdEditor
	{
		// edits arrive from the UI thread, mouse selection from the render thread
		std::recursive_mutex mutex;
		sint32 cursor = 0;
		sint32 anchor = 0; // the selection is the range between anchor and cursor
		std::vector<SwkbdUndoEntry> undo;
		SwkbdEditGroup group = SwkbdEditGroup::None;
		bool dragging = false;
		bool draggingInMainWindow = false; // the keyboard is drawn on both windows
		std::chrono::steady_clock::time_point caretShown{};
		std::shared_ptr<Host::IClipboard> clipboard;
		std::function<void()> stateChanged;
	};

	constexpr size_t kSwkbdUndoLimit = 100;

	SwkbdEditor& Editor()
	{
		static SwkbdEditor editor;
		return editor;
	}

	sint32 SelectionStart()
	{
		return std::min(Editor().cursor, Editor().anchor);
	}

	sint32 SelectionEnd()
	{
		return std::max(Editor().cursor, Editor().anchor);
	}

	bool HasSelection()
	{
		return Editor().cursor != Editor().anchor;
	}

	void ShowCaret()
	{
		Editor().caretShown = std::chrono::steady_clock::now();
	}

	void ResetEditor()
	{
		auto& editor = Editor();
		editor.cursor = editor.anchor = swkbdInternalState->formStringLength;
		editor.undo.clear();
		editor.group = SwkbdEditGroup::None;
		editor.dragging = false;
		ShowCaret();
	}

	void ClampEditor()
	{
		auto& editor = Editor();
		const sint32 length = swkbdInternalState->formStringLength;
		editor.cursor = std::clamp(editor.cursor, 0, length);
		editor.anchor = std::clamp(editor.anchor, 0, length);
	}

	void NotifyStateChanged()
	{
		std::function<void()> callback;
		{
			std::scoped_lock lock(Editor().mutex);
			callback = Editor().stateChanged;
		}
		if (callback)
			callback();
	}

	sint32 MaxTextLength()
	{
		sint32 maxLength = swkbdInternalState->maxTextLength;
		if (swkbdInternalState->keyboardOnlyMode)
		{
			uint32 stringBufferSize = swkbdInternalState->keyboardArg.receiverArg.stringBufSize;
			if (stringBufferSize > 1)
			{
				maxLength = stringBufferSize - 1; // don't count the null-termination character
			}
			else
				maxLength = 0;
		}
		return std::clamp(maxLength, 0, SWKBD_FORM_STRING_MAX_LENGTH - 1);
	}

	std::wstring CurrentText()
	{
		return {swkbdInternalState->formStringBuffer,
				static_cast<size_t>(swkbdInternalState->formStringLength)};
	}

	void StoreText(std::wstring_view text)
	{
		const auto length = std::min<size_t>(text.size(), SWKBD_FORM_STRING_MAX_LENGTH - 1);
		std::copy_n(text.data(), length, swkbdInternalState->formStringBuffer);
		swkbdInternalState->formStringBuffer[length] = '\0';
		swkbdInternalState->formStringLength = static_cast<sint32>(length);
	}

	void NotifyTextChanged()
	{
		swkbd_inputStringChanged();
		PublishSoftwareKeyboard();
	}

	// consecutive edits of the same group (typing, erasing) are undone together
	void PushUndo(SwkbdEditGroup group, bool forceNew)
	{
		auto& editor = Editor();
		if (!forceNew && group != SwkbdEditGroup::None && group == editor.group)
			return;
		editor.group = group;
		editor.undo.push_back({CurrentText(), editor.cursor, editor.anchor});
		if (editor.undo.size() > kSwkbdUndoLimit)
			editor.undo.erase(editor.undo.begin());
	}

	// replaces [start, end) with insert (cut to the length limit) and leaves the caret after it
	bool ApplyEdit(sint32 start, sint32 end, std::wstring_view insert, SwkbdEditGroup group)
	{
		const sint32 length = swkbdInternalState->formStringLength;
		const sint32 room = std::max(0, MaxTextLength() - (length - (end - start)));
		insert = insert.substr(0, std::min<size_t>(insert.size(), room));
		if (insert.empty() && start == end)
			return false;
		auto& editor = Editor();
		PushUndo(group, HasSelection());
		std::wstring text = CurrentText();
		text.replace(start, end - start, insert);
		StoreText(text);
		editor.cursor = editor.anchor = start + static_cast<sint32>(insert.size());
		ShowCaret();
		NotifyTextChanged();
		return true;
	}

	void PasteText(std::uint64_t generation, std::string_view utf8)
	{
		std::scoped_lock lock(Editor().mutex);
		if (swkbdInternalState == nullptr || !swkbdInternalState->isActive ||
			generation != swkbdOverlayGeneration)
			return;
		ClampEditor();
		std::wstring text;
		for (const wchar_t c : boost::nowide::widen(std::string(utf8)))
			if (c >= 0x20 && c != 0x7F) // the form is a single line
				text.push_back(c);
		Editor().group = SwkbdEditGroup::None;
		ApplyEdit(SelectionStart(), SelectionEnd(), text, SwkbdEditGroup::None);
	}

	bool IsWordCharacter(wchar_t c)
	{
		return c != ' ' && c != 0x3000;
	}
} // namespace

void SwkbdCreate(uint8* workMemory, uint32 regionType, uint32 unk, coreinit::FSClient_t* fsClient)
{
	if (swkbdInternalState == nullptr)
	{
		MPTR swkbdInternalStateMPTR = coreinit_allocFromSysArea(sizeof(swkbdInternalState_t), 4);
		swkbdInternalState = (swkbdInternalState_t*)memory_getPointerFromVirtualOffset(swkbdInternalStateMPTR);
		memset(swkbdInternalState, 0x00, sizeof(swkbdInternalState_t));
	}
	coreinit::OSSleepTicks(ESPRESSO_TIMER_CLOCK); // simulate this function taking a bit of time to run. MH3U/MH3G requires this to not softlock
	// pretend that there is some work to do
	isNeedCalcSubThreadFont = 3;
	isNeedCalcSubThreadPredict = 3;
}

uint32 SwkbdGetStateKeyboard()
{
	uint32 r = SWKBD_STATE_BLANK;
	if (swkbdInternalState->isActive)
		r = SWKBD_STATE_DISPLAYED;
	return r;
}

uint32 SwkbdGetStateInputForm()
{
	uint32 r = SWKBD_STATE_BLANK;
	if (swkbdInternalState->isActive)
		r = SWKBD_STATE_DISPLAYED;
	return r;
}

// ReceiverArg:
//+0x00	IEventReceiver*
//+0x04	stringBuf
//+ 0x08	stringBufSize
//+ 0x0C	fixedCharNumLimit(-1)
//+ 0x10	cursorPos
//+ 0x14	selectFrom(-1)
//
// IEventReceiver:
//+0x00 IEventReceiver_vTable*
//+0x04 ?
//+0x08 ?
//+0x0C ?
//
// IEventReceiver_vTable :
//	+0x00 ?
//	+0x04 ?
//	+0x08 ?
//	+0x0C	functionPtr onDirtyString(const DirtyInfo& info) = 0; ->DirtyInfo is just two DWORDs.From and to ?
//	?

struct swkdbIEventReceiver_t
{
	MPTR vTable; // guessed
};

struct swkdbIEventReceiverVTable_t
{
	uint32 ukn00;
	uint32 ukn04;
	uint32 ukn08;
	MPTR onDirtyString;
};

uint32 SwkbdSetReceiver(const swkbdReceiverArg_t* receiverArg)
{
	if (swkbdInternalState == nullptr)
		return 0;
	swkbdInternalState->keyboardArg.receiverArg = *receiverArg;
	return 0;
}

typedef struct
{
	/* +0x00 */ uint32 ukn00;
	/* +0x04 */ uint32 ukn04;
	/* +0x08 */ uint32 ukn08;
	/* +0x0C */ uint32 ukn0C;
	/* +0x10 */ uint32 ukn10;
	/* +0x14 */ uint32 ukn14;
	/* +0x18 */ uint32 ukn18;
	/* +0x1C */ uint32 ukn1C; // pointer to OK string
	/* +0x20 */ uint32 ukn20[4];
	/* +0x30 */ uint32 ukn30[4];
	/* +0x40 */ uint32 ukn40[4];
	/* +0x50 */ uint32 ukn50[4];
	/* +0x60 */ uint32 ukn60[4];
	/* +0x70 */ uint32 ukn70[4];
	/* +0x80 */ uint32 ukn80[4];
	/* +0x90 */ uint32 ukn90[4];
	/* +0xA0 */ uint32 uknA0[4];
	/* +0xB0 */ uint32 uknB0[4];
	/* +0xC0 */ uint32 inputFormType;
	/* +0xC4 */ uint32be cursorIndex;
	/* +0xC8 */ MEMPTR<uint16be> initialText;
	/* +0xCC */ MEMPTR<uint16be> infoText;
	/* +0xD0 */ uint32be maxTextLength;
} swkbdAppearArg_t;

static_assert(offsetof(swkbdAppearArg_t, cursorIndex) == 0xC4, "appearArg.cursorIndex has invalid offset");

uint32 SwkbdAppearInputForm(const swkbdAppearArg_t* appearArg)
{
	++swkbdOverlayGeneration;
	swkbdInternalState->formStringLength = 0;
	swkbdInternalState->isActive = true;
	swkbdInternalState->decideButtonWasPressed = false;
	swkbdInternalState->keyboardOnlyMode = false;

	// setup max text length
	swkbdInternalState->maxTextLength = (sint32)(uint32)appearArg->maxTextLength;
	if (swkbdInternalState->maxTextLength <= 0)
		swkbdInternalState->maxTextLength = SWKBD_FORM_STRING_MAX_LENGTH - 1;
	else
		swkbdInternalState->maxTextLength = std::min(swkbdInternalState->maxTextLength, SWKBD_FORM_STRING_MAX_LENGTH - 1);
	// setup initial string
	uint16be* initialString = appearArg->initialText.GetPtr();
	if (initialString)
	{
		swkbdInternalState->formStringLength = 0;
		for (sint32 i = 0; i < swkbdInternalState->maxTextLength; i++)
		{
			wchar_t c = (uint16)initialString[i];
			if (c == '\0')
				break;
			swkbdInternalState->formStringBuffer[i] = c;
			swkbdInternalState->formStringLength++;
		}
	}
	else
	{
		swkbdInternalState->formStringBuffer[0] = '\0';
		swkbdInternalState->formStringLength = 0;
	}
	{
		std::scoped_lock lock(Editor().mutex);
		ResetEditor();
	}
	PublishSoftwareKeyboard();
	NotifyStateChanged();
	return 1;
}

uint32 SwkbdAppearKeyboard(const SwkbdKeyboardArg_t* keyboardArg)
{
	++swkbdOverlayGeneration;
	// todo: Figure out what the difference between AppearInputForm and AppearKeyboard is?
	uint32 argPtr = MEMPTR(keyboardArg).GetMPTR();
	for (sint32 i = 0; i < 0x180; i += 4)
	{
		debug_printf("+0x%03x: 0x%08x\n", i, memory_readU32(argPtr + i));
	}

	swkbdInternalState->formStringLength = 0;
	swkbdInternalState->isActive = true;
	swkbdInternalState->keyboardOnlyMode = true;
	swkbdInternalState->decideButtonWasPressed = false;
	swkbdInternalState->formStringBuffer[0] = '\0';
	swkbdInternalState->formStringLength = 0;
	swkbdInternalState->keyboardArg = *keyboardArg;
	{
		std::scoped_lock lock(Editor().mutex);
		ResetEditor();
	}
	PublishSoftwareKeyboard();
	NotifyStateChanged();
	return 1;
}

uint32 SwkbdDisappearInputForm()
{
	debug_printf("SwkbdDisappearInputForm__3RplFv\n");
	{
		std::scoped_lock lock(Editor().mutex);
		swkbdInternalState->isActive = false;
		Editor().dragging = false;
	}
	PublishSoftwareKeyboard();
	NotifyStateChanged();
	return 1;
}

uint32 SwkbdDisappearKeyboard()
{
	debug_printf("SwkbdDisappearKeyboard__3RplFv\n");
	{
		std::scoped_lock lock(Editor().mutex);
		swkbdInternalState->isActive = false;
		Editor().dragging = false;
	}
	PublishSoftwareKeyboard();
	NotifyStateChanged();
	return 1;
}

uint16be* SwkbdGetInputFormString()
{
	std::scoped_lock lock(Editor().mutex);
	for (sint32 i = 0; i < swkbdInternalState->formStringLength; i++)
		swkbdInternalState->formStringBufferBE[i] = swkbdInternalState->formStringBuffer[i];
	cemu_assert(swkbdInternalState->formStringLength < SWKBD_FORM_STRING_MAX_LENGTH);
	swkbdInternalState->formStringBufferBE[swkbdInternalState->formStringLength] = 0;
	return swkbdInternalState->formStringBufferBE;
}

uint32 SwkbdIsDecideOkButton(uint8*)
{
	if (swkbdInternalState->decideButtonWasPressed)
		return 1;
	return 0;
}

typedef struct
{
	uint32be ukn00;
	uint32be ukn04;
	uint32be ukn08;
	uint32be ukn0C;
	uint32be ukn10;
	uint32be ukn14;
	uint8 ukn18;
	// there might be padding here?
} SwkbdDrawStringInfo_t;

static_assert(sizeof(SwkbdDrawStringInfo_t) != 0x19, "SwkbdDrawStringInfo_t has invalid size");

uint32 SwkbdGetDrawStringInfo(SwkbdDrawStringInfo_t* drawStringInfo)
{
	cemuLog_logDebug(LogType::Force, "SwkbdGetDrawStringInfo(0x{:08x})", MEMPTR(drawStringInfo).GetMPTR());

	drawStringInfo->ukn00 = -1;
	drawStringInfo->ukn04 = -1;
	drawStringInfo->ukn08 = -1;
	drawStringInfo->ukn0C = -1;
	drawStringInfo->ukn10 = -1;
	drawStringInfo->ukn14 = -1;
	drawStringInfo->ukn18 = 0;

	return 0;
}

constexpr uint32 SWKBD_LEARN_DIC_SIZE = 0xA460;
constexpr uint32 SWKBD_LEARN_DIC_ENTRY_COUNT = 1000;
constexpr uint32 SWKBD_LEARN_DIC_ENTRY_STRIDE = 0x20;
constexpr uint32 SWKBD_LEARN_DIC_INDEX_COUNT = SWKBD_LEARN_DIC_ENTRY_COUNT + 1;
constexpr uint32 SWKBD_LEARN_DIC_MAGIC = 0x4E4A4443; // "NJDC"

struct SwkbdLearnDic
{
	/* 0x00 */ uint32be magic;
	/* 0x04 */ uint32be version;
	/* 0x08 */ uint32be format;
	/* 0x0C */ uint32be storageSize;
	/* 0x10 */ uint32be metadataSize;
	/* 0x14 */ uint32be maxWordLength;
	/* 0x18 */ uint32be maxReadingLength;
	/* 0x1C */ uint32be indexTableEnd;
	/* 0x20 */ uint32be entryBaseOffset;
	/* 0x24 */ uint32be reserved24;
	/* 0x28 */ uint32be entryCount;
	/* 0x2C */ uint32be entryStride;
	/* 0x30 */ uint32be reserved30;
	/* 0x34 */ uint32be reserved34;
	/* 0x38 */ uint32be reserved38;
	/* 0x3C */ uint32be firstIndexOffset;
	/* 0x40 */ uint32be secondIndexOffset;
	/* 0x44 */ uint32be entryAreaEnd;
	/* 0x48 */ uint8 payload[SWKBD_LEARN_DIC_SIZE - 0x48 - sizeof(uint32be)];
	/* 0xA45C */ uint32be endMagic;
};
static_assert(sizeof(SwkbdLearnDic) == SWKBD_LEARN_DIC_SIZE);

uint32 SwkbdInitLearnDic(SwkbdLearnDic* dictionary)
{
	cemuLog_logDebug(LogType::Force, "SwkbdInitLearnDic(0x{:08x})", MEMPTR(dictionary).GetMPTR());
	if (swkbdInternalState == nullptr || dictionary == nullptr)
		return 0;

	uint32 firstIndexOffset = 0x48;
	uint32 secondIndexOffset = firstIndexOffset + SWKBD_LEARN_DIC_INDEX_COUNT * sizeof(uint16be);
	uint32 indexTableEnd = secondIndexOffset + SWKBD_LEARN_DIC_INDEX_COUNT * sizeof(uint16be);
	uint32 entryAreaEnd = indexTableEnd + SWKBD_LEARN_DIC_ENTRY_COUNT * SWKBD_LEARN_DIC_ENTRY_STRIDE;

	memset(dictionary, 0, sizeof(SwkbdLearnDic));
	dictionary->magic = SWKBD_LEARN_DIC_MAGIC;
	dictionary->version = 0x30000;
	dictionary->format = 0x80020000;
	dictionary->storageSize = SWKBD_LEARN_DIC_SIZE - 0x48;
	dictionary->metadataSize = 0x2C;
	dictionary->maxWordLength = 0x6E;
	dictionary->maxReadingLength = 0x6E;
	dictionary->indexTableEnd = indexTableEnd;
	dictionary->entryCount = SWKBD_LEARN_DIC_ENTRY_COUNT;
	dictionary->entryStride = SWKBD_LEARN_DIC_ENTRY_STRIDE;
	dictionary->firstIndexOffset = firstIndexOffset;
	dictionary->secondIndexOffset = secondIndexOffset;
	dictionary->entryAreaEnd = entryAreaEnd;
	dictionary->endMagic = SWKBD_LEARN_DIC_MAGIC;

	return 1;
}

bool SwkbdIsNeedCalcSubThreadFont()
{
	return isNeedCalcSubThreadFont > 0;
}

bool SwkbdIsNeedCalcSubThreadPredict()
{
	return isNeedCalcSubThreadPredict > 0;
}

void SwkbdCalcSubThreadFont()
{
	if (isNeedCalcSubThreadFont == 0)
		return;
	isNeedCalcSubThreadFont--;
	coreinit::OSSleepTicks(ESPRESSO_TIMER_CLOCK / 200); // simulate 5ms of working time
}

void SwkbdCalcSubThreadPredict()
{
	if (isNeedCalcSubThreadPredict == 0)
		return;
	isNeedCalcSubThreadPredict--;
	coreinit::OSSleepTicks(ESPRESSO_TIMER_CLOCK / 200); // simulate 5ms of working time
}

void SwkbdCalc(void* controllerInfo)
{
	// no op for now
}

void swkbd_keyInput(uint32 keyCode);
#if defined(CEMU_OVERLAY_BACKEND_IMGUI)
namespace
{
	// draws the form text with its selection and caret, and handles mouse selection
	void DrawSwkbdText(bool mainWindow)
	{
		auto& editor = Editor();
		ClampEditor();
		const wchar_t* text = swkbdInternalState->formStringBuffer;
		const sint32 length = swkbdInternalState->formStringLength;
		ImFont* font = ImGui::GetFont();
		const float fontSize = ImGui::GetFontSize();
		const float scale = fontSize / font->FontSize;
		const float lineHeight = ImGui::GetTextLineHeight();
		const float wrapWidth = std::max(ImGui::GetContentRegionAvail().x, fontSize);

		// lay out per character so that long text wraps like before
		std::vector<float> x(length + 1), advance(length + 1, 0.0f);
		std::vector<sint32> line(length + 1);
		std::vector<sint32> lineStarts{0};
		float pen = 0.0f;
		for (sint32 i = 0; i < length; i++)
		{
			advance[i] = font->GetCharAdvance((ImWchar)text[i]) * scale;
			if (pen > 0.0f && pen + advance[i] > wrapWidth)
			{
				lineStarts.push_back(i);
				pen = 0.0f;
			}
			x[i] = pen;
			line[i] = (sint32)lineStarts.size() - 1;
			pen += advance[i];
		}
		x[length] = pen;
		line[length] = (sint32)lineStarts.size() - 1;
		const sint32 lineCount = (sint32)lineStarts.size();
		auto lineEnd = [&](sint32 l) { return l + 1 < lineCount ? lineStarts[l + 1] : length; };

		const ImVec2 origin = ImGui::GetCursorScreenPos();
		ImGui::Dummy({wrapWidth, lineHeight * lineCount});

		auto hitTest = [&](const ImVec2& mouse) {
			const sint32 l = std::clamp((sint32)std::floor((mouse.y - origin.y) / lineHeight), 0, lineCount - 1);
			const sint32 end = lineEnd(l);
			for (sint32 i = lineStarts[l]; i < end; i++)
			{
				if (mouse.x < origin.x + x[i] + advance[i] * 0.5f)
					return i;
			}
			return end;
		};

		// the whole input window acts as the text area
		const auto& io = ImGui::GetIO();
		const bool hovered = ImGui::IsWindowHovered();
		if (hovered)
			ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
		if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		{
			const sint32 hit = hitTest(io.MousePos);
			editor.group = SwkbdEditGroup::None;
			if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			{
				// select the word under the pointer
				sint32 start = std::min(hit, std::max(0, length - 1));
				if (length > 0 && !IsWordCharacter(text[start]) && start > 0 && IsWordCharacter(text[start - 1]))
					start--;
				sint32 end = start;
				if (length > 0 && IsWordCharacter(text[start]))
				{
					while (start > 0 && IsWordCharacter(text[start - 1]))
						start--;
					while (end < length && IsWordCharacter(text[end]))
						end++;
				}
				editor.anchor = start;
				editor.cursor = end;
				editor.dragging = false;
			}
			else
			{
				editor.cursor = editor.anchor = hit;
				editor.dragging = true;
				editor.draggingInMainWindow = mainWindow;
			}
			ShowCaret();
		}
		else if (editor.dragging && editor.draggingInMainWindow == mainWindow)
		{
			if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
				editor.cursor = hitTest(io.MousePos);
			else
				editor.dragging = false;
		}

		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const sint32 selectionStart = SelectionStart();
		const sint32 selectionEnd = SelectionEnd();
		const ImU32 selectionColor = IM_COL32(0x26, 0x7D, 0xF2, 0xFF);
		const ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);
		for (sint32 l = 0; l < lineCount; l++)
		{
			const sint32 begin = lineStarts[l];
			const sint32 end = lineEnd(l);
			const float top = origin.y + lineHeight * l;
			const sint32 from = std::max(selectionStart, begin);
			const sint32 to = std::min(selectionEnd, end);
			if (from < to)
				drawList->AddRectFilled({origin.x + x[from], top},
										{origin.x + x[to - 1] + advance[to - 1], top + lineHeight},
										selectionColor);
			const auto utf8 = boost::nowide::narrow(std::wstring(text + begin, text + end));
			drawList->AddText(font, fontSize, {origin.x, top}, textColor, utf8.c_str(), utf8.c_str() + utf8.size());
		}

		// caret blinks, but stays visible right after it moved
		const auto shownFor = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - editor.caretShown).count();
		if (shownFor % 1000 < 500)
		{
			const float caretX = origin.x + x[editor.cursor];
			const float caretY = origin.y + lineHeight * line[editor.cursor];
			drawList->AddRectFilled({caretX, caretY}, {caretX + 2.0f, caretY + lineHeight}, textColor);
		}
	}
} // namespace

void swkbd_render(bool mainWindow)
{
	std::scoped_lock lock(Editor().mutex);
	// only render if active
	if (swkbdInternalState == NULL || swkbdInternalState->isActive == false)
		return;

	// Japanese and other non-Latin input needs its glyphs baked before it can be shown instead of '?'
	ImGui_RequestGlyphs(swkbdInternalState->formStringBuffer);

	auto& io = ImGui::GetIO();
	const auto font = ImGui_GetFont(48.0f);
	const auto textFont = ImGui_GetFont(24.0f);
	if (!font || !textFont)
		return;

	const auto kPopupFlags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;

	ImGui::PushStyleColor(ImGuiCol_WindowBg, 0);

	ImGui::SetNextWindowPos({0, 0}, ImGuiCond_Always);
	ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
	ImGui::SetNextWindowBgAlpha(0.8f);
	ImGui::Begin("Background overlay", nullptr, kPopupFlags | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus);
	ImGui::End();
	ImGui::PopStyleVar(2);

	ImVec2 position = {io.DisplaySize.x / 2.0f, io.DisplaySize.y / 3.0f};
	ImVec2 pivot = {0.5f, 0.5f};

	const auto button_len = font->GetCharAdvance('W');
	const float len = button_len * std::max(4, std::max(swkbdInternalState->maxTextLength, (sint32)swkbdInternalState->keyboardArg.receiverArg.stringBufSize));

	ImVec2 box_size = {std::min(io.DisplaySize.x * 0.9f, len + 90), 0};
	ImGui::SetNextWindowPos(position, ImGuiCond_Always, pivot);
	ImGui::SetNextWindowSize(box_size, ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.9f);
	ImGui::PushFont(font);
	if (ImGui::Begin("Keyboard Input", nullptr, kPopupFlags))
	{
		ImGui::Text("%s", _utf8WrapperPtr(ICON_FA_KEYBOARD));
		ImGui::SameLine(70);
		DrawSwkbdText(mainWindow);

		position.y += ImGui::GetWindowSize().y + 100.0f;
	}
	ImGui::End();
	ImGui::PopFont();

	ImGui::SetNextWindowPos(position, ImGuiCond_Always, pivot);
	// ImGui::SetNextWindowSize({ io.DisplaySize.x * 0.9f , 0.0f}, ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.9f);
	ImGui::PushFont(textFont);

	if (ImGui::Begin(fmt::format("Software keyboard##SoftwareKeyboard{}", mainWindow).c_str(), nullptr, kPopupFlags))
	{
		if (swkbdInternalState->shiftActivated)
		{
			const char* keys[] =
				{
					"#", "[", "]", "$", "%", "^", "&", "*", "(", ")", "_", _utf8WrapperPtr(ICON_FA_ARROW_CIRCLE_LEFT), "\n",
					"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "@", "\n",
					"A", "S", "D", "F", "G", "H", "J", "K", "L", ";", "\"", "\n",
					"Z", "X", "C", "V", "B", "N", "M", "<", ">", "+", "=", "\n",
					_utf8WrapperPtr(ICON_FA_ARROW_UP), " ", _utf8WrapperPtr(ICON_FA_CHECK)};
			for (auto key : keys)
			{
				if (*key != '\n')
				{
					if (ImGui::Button(key, {*key == ' ' ? 537 : (button_len + 5), 0}))
					{
						if (strcmp(key, _utf8WrapperPtr(ICON_FA_ARROW_CIRCLE_LEFT)) == 0)
							swkbd_keyInput(8);
						else if (strcmp(key, _utf8WrapperPtr(ICON_FA_ARROW_UP)) == 0)
							swkbdInternalState->shiftActivated = !swkbdInternalState->shiftActivated;
						else if (strcmp(key, _utf8WrapperPtr(ICON_FA_CHECK)) == 0)
							swkbd_keyInput(13);
						else
							swkbd_keyInput(*key);
					}

					ImGui::SameLine();
				}
				else
					ImGui::NewLine();
			}
		}
		else
		{
			const char* keys[] =
				{
					"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", _utf8WrapperPtr(ICON_FA_ARROW_CIRCLE_LEFT), "\n",
					"q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "/", "\n",
					"a", "s", "d", "f", "g", "h", "j", "k", "l", ":", "'", "\n",
					"z", "x", "c", "v", "b", "n", "m", ",", ".", "?", "!", "\n",
					_utf8WrapperPtr(ICON_FA_ARROW_UP), " ", _utf8WrapperPtr(ICON_FA_CHECK)};
			for (auto key : keys)
			{
				if (*key != '\n')
				{
					if (ImGui::Button(key, {*key == ' ' ? 537 : (button_len + 5), 0}))
					{
						if (strcmp(key, _utf8WrapperPtr(ICON_FA_ARROW_CIRCLE_LEFT)) == 0)
							swkbd_keyInput(8);
						else if (strcmp(key, _utf8WrapperPtr(ICON_FA_ARROW_UP)) == 0)
							swkbdInternalState->shiftActivated = !swkbdInternalState->shiftActivated;
						else if (strcmp(key, _utf8WrapperPtr(ICON_FA_CHECK)) == 0)
							swkbd_keyInput(13);
						else
							swkbd_keyInput(*key);
					}

					ImGui::SameLine();
				}
				else
					ImGui::NewLine();
			}
		}
		ImGui::NewLine();
	}
	ImGui::End();

	if (io.NavInputs[ImGuiNavInput_Cancel] > 0)
	{
		if (!swkbdInternalState->cancelState)
			swkbd_keyInput(8); // backspace
		swkbdInternalState->cancelState = true;
	}
	else
		swkbdInternalState->cancelState = false;

	if (io.NavInputs[ImGuiNavInput_Input] > 0)
	{
		if (!swkbdInternalState->returnState)
			swkbd_keyInput(13); // return
		swkbdInternalState->returnState = true;
	}
	else
		swkbdInternalState->returnState = false;

	ImGui::PopFont();
	ImGui::PopStyleColor();
}
#else
void swkbd_render(bool)
{
}
#endif

bool swkbd_hasKeyboardInputHook()
{
	return swkbdInternalState != NULL && swkbdInternalState->isActive;
}

std::uint64_t swkbd_overlayGeneration()
{
	return swkbdOverlayGeneration;
}

void swkbd_setClipboard(std::shared_ptr<Host::IClipboard> clipboard)
{
	std::scoped_lock lock(Editor().mutex);
	Editor().clipboard = std::move(clipboard);
}

void swkbd_setStateChangedCallback(std::function<void()> callback)
{
	std::scoped_lock lock(Editor().mutex);
	Editor().stateChanged = std::move(callback);
}

void swkbd_finishInput()
{
	swkbdInternalState->decideButtonWasPressed = true; // currently we always accept the input
}

typedef struct
{
	uint32be beginIndex;
	uint32be endIndex;
} changeStringParam_t;

SysAllocator<changeStringParam_t> _changeStringParam;

void swkbd_inputStringChanged()
{
	if (true) // swkbdInternalState->keyboardOnlyMode )
	{
		// write changed string to application's string buffer
		uint32 stringBufferSize = swkbdInternalState->keyboardArg.receiverArg.stringBufSize; // in 2-byte words
		if (stringBufferSize > 1)
		{
			stringBufferSize--; // don't count the null-termination character
			const auto stringBufferBE = swkbdInternalState->keyboardArg.receiverArg.stringBuf.GetPtr();
			sint32 copyLength = std::min((sint32)stringBufferSize, swkbdInternalState->formStringLength);
			for (sint32 i = 0; i < copyLength; i++)
			{
				stringBufferBE[i] = swkbdInternalState->formStringBuffer[i];
			}
			stringBufferBE[copyLength] = '\0';

			// swkbdInternalState->keyboardArg.cursorPos = copyLength;
		}
		// IEventReceiver callback
		if (swkbdInternalState->keyboardArg.receiverArg.IEventReceiver)
		{
			SwkbdIEventReceiver_t* eventReceiver = swkbdInternalState->keyboardArg.receiverArg.IEventReceiver.GetPtr();
			MPTR cbChangeString = eventReceiver->vTable->changeString.GetMPTR();
			if (cbChangeString)
			{
				changeStringParam_t* changeStringParam = _changeStringParam.GetPtr();
				changeStringParam->beginIndex = 0;
				changeStringParam->endIndex = 0;
				coreinitAsyncCallback_add(cbChangeString, 2, memory_getVirtualOffsetFromPointer(eventReceiver), _changeStringParam.GetMPTR());
			}
		}
	}
}

void swkbd_keyInput(uint32 keyCode)
{
	std::scoped_lock lock(Editor().mutex);
	if (swkbdInternalState == nullptr)
		return;
	ClampEditor();
	// Ctrl+letter arrives as its control character
	switch (keyCode)
	{
	case 1: // Ctrl+A
		swkbd_editCommand(SwkbdEditCommand::SelectAll, false);
		return;
	case 3: // Ctrl+C
		swkbd_editCommand(SwkbdEditCommand::Copy, false);
		return;
	case 22: // Ctrl+V
		swkbd_editCommand(SwkbdEditCommand::Paste, false);
		return;
	case 24: // Ctrl+X
		swkbd_editCommand(SwkbdEditCommand::Cut, false);
		return;
	case 26: // Ctrl+Z
		swkbd_editCommand(SwkbdEditCommand::Undo, false);
		return;
	}
	if (keyCode == 8 || keyCode == 127) // backspace || backwards delete
	{
		if (HasSelection())
			ApplyEdit(SelectionStart(), SelectionEnd(), {}, SwkbdEditGroup::None);
		else if (Editor().cursor > 0)
			ApplyEdit(Editor().cursor - 1, Editor().cursor, {}, SwkbdEditGroup::Erasing);
		return;
	}
	else if (keyCode == 13) // return
	{
		swkbd_finishInput();
		PublishSoftwareKeyboard();
		return;
	}
	else if (keyCode < 32)
	{
		// control key
		return;
	}
	// typing replaces the selection
	const wchar_t character = static_cast<wchar_t>(keyCode);
	ApplyEdit(SelectionStart(), SelectionEnd(), {&character, 1}, SwkbdEditGroup::Typing);
}

void swkbd_editCommand(SwkbdEditCommand command, bool extendSelection)
{
	std::shared_ptr<Host::IClipboard> clipboard;
	std::string copied;
	bool paste = false;
	std::uint64_t generation = 0;
	{
		std::scoped_lock lock(Editor().mutex);
		if (swkbdInternalState == nullptr || !swkbdInternalState->isActive)
			return;
		ClampEditor();
		auto& editor = Editor();
		const sint32 length = swkbdInternalState->formStringLength;
		auto moveCaret = [&](sint32 position) {
			editor.cursor = std::clamp(position, 0, length);
			if (!extendSelection)
				editor.anchor = editor.cursor;
			editor.group = SwkbdEditGroup::None;
			ShowCaret();
		};
		switch (command)
		{
		case SwkbdEditCommand::SelectAll:
			editor.anchor = 0;
			editor.cursor = length;
			editor.group = SwkbdEditGroup::None;
			ShowCaret();
			break;
		case SwkbdEditCommand::Copy:
		case SwkbdEditCommand::Cut:
			if (!HasSelection() || !editor.clipboard)
				return;
			clipboard = editor.clipboard;
			copied = boost::nowide::narrow(CurrentText().substr(SelectionStart(), SelectionEnd() - SelectionStart()));
			if (command == SwkbdEditCommand::Cut)
				ApplyEdit(SelectionStart(), SelectionEnd(), {}, SwkbdEditGroup::None);
			break;
		case SwkbdEditCommand::Paste:
			clipboard = editor.clipboard;
			paste = true;
			generation = swkbdOverlayGeneration;
			break;
		case SwkbdEditCommand::Undo:
		{
			if (editor.undo.empty())
				return;
			const auto entry = std::move(editor.undo.back());
			editor.undo.pop_back();
			StoreText(entry.text);
			editor.cursor = entry.cursor;
			editor.anchor = entry.anchor;
			editor.group = SwkbdEditGroup::None;
			ClampEditor();
			ShowCaret();
			NotifyTextChanged();
			break;
		}
		case SwkbdEditCommand::CursorLeft:
			moveCaret(HasSelection() && !extendSelection ? SelectionStart() : editor.cursor - 1);
			break;
		case SwkbdEditCommand::CursorRight:
			moveCaret(HasSelection() && !extendSelection ? SelectionEnd() : editor.cursor + 1);
			break;
		case SwkbdEditCommand::CursorHome:
			moveCaret(0);
			break;
		case SwkbdEditCommand::CursorEnd:
			moveCaret(length);
			break;
		case SwkbdEditCommand::DeleteForward:
			if (HasSelection())
				ApplyEdit(SelectionStart(), SelectionEnd(), {}, SwkbdEditGroup::None);
			else if (editor.cursor < length)
				ApplyEdit(editor.cursor, editor.cursor + 1, {}, SwkbdEditGroup::Erasing);
			break;
		}
	}
	// the clipboard completes on the UI thread, so it is used outside of the lock
	if (clipboard && !copied.empty())
		clipboard->SetTextAsync(std::move(copied), [](bool) {});
	if (clipboard && paste)
		clipboard->GetTextAsync([generation](bool success, std::string text) {
			if (success)
				PasteText(generation, text);
		});
}

namespace swkbd
{
	class : public COSModule
	{
	  public:
		std::string_view GetName() override
		{
			return "swkbd";
		}

		void RPLMapped() override
		{
			cafeExportRegisterFunc(SwkbdCreate, "swkbd", "SwkbdCreate__3RplFPUcQ3_2nn5swkbd10RegionTypeUiP8FSClient", LogType::SWKBD);
			cafeExportRegisterFunc(SwkbdCalc, "swkbd", "SwkbdCalc__3RplFRCQ3_2nn5swkbd14ControllerInfo", LogType::SWKBD);
			cafeExportRegisterFunc(SwkbdGetStateKeyboard, "swkbd", "SwkbdGetStateKeyboard__3RplFv", LogType::SWKBD);
			cafeExportRegisterFunc(SwkbdGetStateInputForm, "swkbd", "SwkbdGetStateInputForm__3RplFv", LogType::SWKBD);
			cafeExportRegisterFunc(SwkbdSetReceiver, "swkbd", "SwkbdSetReceiver__3RplFRCQ3_2nn5swkbd11ReceiverArg", LogType::SWKBD);
			cafeExportRegisterFunc(SwkbdAppearInputForm, "swkbd", "SwkbdAppearInputForm__3RplFRCQ3_2nn5swkbd9AppearArg", LogType::SWKBD);
			cafeExportRegisterFunc(SwkbdDisappearInputForm, "swkbd", "SwkbdDisappearInputForm__3RplFv", LogType::SWKBD);
			cafeExportRegisterFunc(SwkbdDisappearKeyboard, "swkbd", "SwkbdDisappearKeyboard__3RplFv", LogType::SWKBD);
			cafeExportRegisterFunc(SwkbdAppearKeyboard, "swkbd", "SwkbdAppearKeyboard__3RplFRCQ3_2nn5swkbd11KeyboardArg", LogType::SWKBD);
			cafeExportRegisterFunc(SwkbdGetInputFormString, "swkbd", "SwkbdGetInputFormString__3RplFv", LogType::SWKBD);
			cafeExportRegisterFunc(SwkbdIsDecideOkButton, "swkbd", "SwkbdIsDecideOkButton__3RplFPb", LogType::SWKBD);
			cafeExportRegisterFunc(SwkbdInitLearnDic, "swkbd", "SwkbdInitLearnDic__3RplFPv", LogType::SWKBD);
			cafeExportRegisterFunc(SwkbdGetDrawStringInfo, "swkbd", "SwkbdGetDrawStringInfo__3RplFPQ3_2nn5swkbd14DrawStringInfo", LogType::SWKBD);
			cafeExportRegisterFunc(SwkbdCalcSubThreadFont, "swkbd", "SwkbdCalcSubThreadFont__3RplFv", LogType::SWKBD);
			cafeExportRegisterFunc(SwkbdCalcSubThreadPredict, "swkbd", "SwkbdCalcSubThreadPredict__3RplFv", LogType::SWKBD);
			cafeExportRegisterFunc(SwkbdIsNeedCalcSubThreadFont, "swkbd", "SwkbdIsNeedCalcSubThreadFont__3RplFv", LogType::SWKBD);
			cafeExportRegisterFunc(SwkbdIsNeedCalcSubThreadPredict, "swkbd", "SwkbdIsNeedCalcSubThreadPredict__3RplFv", LogType::SWKBD);
		};

		void rpl_entry(uint32 moduleHandle, coreinit::RplEntryReason reason) override
		{
			if (reason == coreinit::RplEntryReason::Loaded)
			{
				swkbdInternalState = nullptr;
			}
			else if (reason == coreinit::RplEntryReason::Unloaded)
			{
				// todo
			}
		}
	} s_COSswkbdModule;

	COSModule* GetModule()
	{
		return &s_COSswkbdModule;
	}
} // namespace swkbd
