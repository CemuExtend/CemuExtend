#pragma once
#include "wxgui/components/TextList.h"

class DumpCtrl : public TextList
{
  public:
	DumpCtrl(wxWindow* parent, const wxWindowID& id, const wxPoint& pos, const wxSize& size, long style);

	void Init();
	wxSize DoGetBestSize() const override;

  protected:
	void GoToAddressDialog();
	void CenterOffset(std::uint32_t offset);
	std::uint32_t LineToOffset(std::uint32_t line);
	std::uint32_t OffsetToLine(std::uint32_t offset);
	std::uint32_t PositionToAddress(const wxPoint& position, std::uint32_t line);

	void OnDraw(wxDC& dc, std::int32_t start, std::int32_t count, const wxPoint& start_position) override;
	void OnMouseMove(const wxPoint& position, std::uint32_t line) override;
	void OnMouseDClick(const wxPoint& position, std::uint32_t line) override;
	void OnKeyPressed(std::int32_t key_code, const wxPoint& position) override;
	void OnContextMenu(const wxPoint& position, std::uint32_t line) override;
	void OnMenuSelected(wxCommandEvent& event);

	template<typename T>
	bool WriteNumericDialog(std::uint32_t address);
	bool WriteString(std::uint32_t address);

  private:
	struct
	{
		std::uint32_t baseAddress;
		std::uint32_t size;
	} m_memoryRegion;
	std::uint32_t m_lastGotoOffset{0};
	std::uint32_t m_writerContextAddress{0};
	std::uint32_t m_writerContextLine{0};
};
