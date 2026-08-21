#pragma once

#include "Common/precompiled.h"
#include "util/math/vector3.h"

// Host-side controller samples shared with the Cafe ABI adapters.  These
// structures intentionally contain values only: Input produces them and Cafe
// exposes the same layout to the guest.  Keeping the contract here prevents
// the lower Input target from including Cafe implementation headers.

struct BtnRepeat
{
	sint32 delay, pulse;
};

constexpr sint8 kWpadErrorNone = 0;

struct padVec3D_t
{
	float x;
	float y;
	float z;
};

static_assert(sizeof(padVec3D_t) == 0xC);

struct beVec3D_t
{
	float32be x;
	float32be y;
	float32be z;

	beVec3D_t() = default;

	beVec3D_t(const Vector3<float>& v)
		: x(v.x), y(v.y), z(v.z) {}

	beVec3D_t(const float32be& x, const float32be& y, const float32be& z)
		: x(x), y(y), z(z) {}
};

static_assert(sizeof(beVec3D_t) == 0xC);

struct padVec2D_t
{
	float x;
	float y;
};

static_assert(sizeof(padVec2D_t) == 0x8);

struct beVec2D_t
{
	float32be x;
	float32be y;
};

static_assert(sizeof(beVec2D_t) == 0x8);

union KPADEXStatus_t
{
	struct
	{
		beVec2D_t stick;
		beVec3D_t acc;
		float32be accValue;
		float32be accSpeed;
	} fs;
	struct
	{
		uint32be hold;
		uint32be trig;
		uint32be release;
		beVec2D_t lstick;
		beVec2D_t rstick;
		float32be ltrigger;
		float32be rtrigger;
	} cl;
	struct
	{
		uint32be hold;
		uint32be trig;
		uint32be release;
		beVec2D_t lstick;
		beVec2D_t rstick;
		uint32be charge;
		uint32be cable;
	} uc;
	struct
	{
		uint32be hold;
		uint32be trig;
		uint32be release;
	} cm;
	uint8 _padding[0x50];
};

static_assert(sizeof(KPADEXStatus_t) == 0x50);

struct KPADMPDir_t
{
	padVec3D_t X;
	padVec3D_t Y;
	padVec3D_t Z;
};

static_assert(sizeof(KPADMPDir_t) == 0x24);

struct KPADMPStatus_t
{
	padVec3D_t mpls;
	padVec3D_t angle;
	KPADMPDir_t dir;
};

static_assert(sizeof(KPADMPStatus_t) == 0x3C);

struct KPADStatus_t
{
	uint32be hold;
	uint32be trig;
	uint32be release;
	beVec3D_t acc;
	float32be acc_value;
	float32be acc_speed;
	beVec2D_t pos;
	beVec2D_t vec;
	float32be speed;
	beVec2D_t horizon;
	beVec2D_t horiVec;
	float32be horiSpeed;
	float32be dist;
	float32be distVec;
	float32be distSpeed;
	beVec2D_t accVertical;
	uint8 devType;
	sint8 wpadErr;
	sint8 dpd_valid_fg;
	uint8 data_format;
	KPADEXStatus_t ex_status;
	KPADMPStatus_t mpls;
	uint8 _unused[4];
};

static_assert(sizeof(KPADStatus_t) == 0xF0);

constexpr std::size_t kWpadDpdMaximumObjects = 4;

struct DPDObject_t
{
	uint16be x;
	uint16be y;
	uint16be size;
	uint8 traceId;
	uint8 padding;
};

static_assert(sizeof(DPDObject_t) == 0x8);

struct WPADStatus_t
{
	uint16be button;
	uint16be accX;
	uint16be accY;
	uint16be accZ;
	DPDObject_t obj[kWpadDpdMaximumObjects];
	uint8 dev;
	sint8 err;
};

static_assert(sizeof(WPADStatus_t) == 0x2A);

struct WPADFSStatus_t : WPADStatus_t
{
	uint16be fsAccX;
	uint16be fsAccY;
	uint16be fsAccZ;
	sint8 fsStickX;
	sint8 fsStickY;
};

struct WPADCLStatus_t : WPADStatus_t
{
	uint16be clButton;
	uint16be clLStickX;
	uint16be clLStickY;
	uint16be clRStickX;
	uint16be clRStickY;
	uint8 clTriggerL;
	uint8 clTriggerR;
};

struct WPADUCStatus_t : WPADStatus_t
{
	uint8 padding1[2];
	uint32be ucButton;
	uint16be ucLStickX;
	uint16be ucLStickY;
	uint16be ucRStickX;
	uint16be ucRStickY;
	uint32be charge;
	uint32be cable;
};

static_assert(sizeof(WPADUCStatus_t) == 0x40);

struct WPADTRStatus_t : WPADStatus_t
{
	uint16 trButton;
	uint8 brake;
	uint8 mascon;
};

constexpr std::size_t kWpadPressureUnits = 4;

struct WPADBLStatus_t : WPADStatus_t
{
	uint16be press[kWpadPressureUnits];
	sint8 temp;
	uint8 battery;
};

static_assert(sizeof(WPADBLStatus_t) == 0x34);

struct WPADMPStatus_t : WPADStatus_t
{
	union
	{
		struct
		{
			uint16be fsAccX;
			uint16be fsAccY;
			uint16be fsAccZ;
			sint8 fsStickX;
			sint8 fsStickY;
		} fs;
		struct
		{
			uint16be clButton;
			uint16be clLStickX;
			uint16be clLStickY;
			uint16be clRStickX;
			uint16be clRStickY;
			uint8 clTriggerL;
			uint8 clTriggerR;
		} cl;
	} status;
	uint8 stat;
	uint8 _ukn;
	uint16be pitch;
	uint16be yaw;
	uint16be roll;
};

static_assert(sizeof(WPADMPStatus_t) == 0x3E);

struct KPADUnifiedWpadStatus
{
	union
	{
		WPADStatus_t core;
		WPADFSStatus_t fs;
		WPADCLStatus_t cl;
		WPADTRStatus_t tr;
		WPADBLStatus_t bl;
		WPADMPStatus_t mp;
	} u;
	uint8 fmt;
	uint8 padding;
	uint32 _40;
};

using KPADUnifiedWpadStatus_t = KPADUnifiedWpadStatus;
static_assert(sizeof(KPADUnifiedWpadStatus) == 0x44);

enum VPADTouchValidity
{
	kTpValid = 0,
	kTpInvalidX = 1,
	kTpInvalidY = 2,
	kTpInvalid = kTpInvalidX | kTpInvalidY,
};

enum VPADTouchState
{
	kTpTouchOff = 0,
	kTpTouchOn = 1,
};

struct VPADDir
{
	beVec3D_t x;
	beVec3D_t y;
	beVec3D_t z;

	VPADDir() = default;
	VPADDir(const beVec3D_t& x, const beVec3D_t& y, const beVec3D_t& z)
		: x(x), y(y), z(z) {}
};

static_assert(sizeof(VPADDir) == 0x24);

struct VPADTPData_t
{
	uint16be x;
	uint16be y;
	uint16be touch;
	uint16be validity;
};

static_assert(sizeof(VPADTPData_t) == 8);

struct VPADStatus
{
	/* +0x00 */ uint32be hold;
	/* +0x04 */ uint32be trig;
	/* +0x08 */ uint32be release;
	/* +0x0C */ beVec2D_t leftStick;
	/* +0x14 */ beVec2D_t rightStick;
	/* +0x1C */ beVec3D_t acc;
	/* +0x28 */ float32be accMagnitude;
	/* +0x2C */ float32be accAcceleration;
	/* +0x30 */ beVec2D_t accXY;
	/* +0x38 */ beVec3D_t gyroChange;
	/* +0x44 */ beVec3D_t gyroOrientation;
	/* +0x50 */ sint8 vpadErr;
	/* +0x51 */ uint8 padding1[1];
	/* +0x52 */ VPADTPData_t tpData;
	/* +0x5A */ VPADTPData_t tpProcessed1;
	/* +0x62 */ VPADTPData_t tpProcessed2;
	/* +0x6A */ uint8 padding2[2];
	/* +0x6C */ VPADDir dir;
	/* +0x90 */ uint8 headphoneStatus;
	/* +0x91 */ uint8 padding3[3];
	/* +0x94 */ beVec3D_t magnet;
	/* +0xA0 */ uint8 slideVolume;
	/* +0xA1 */ uint8 batteryLevel;
	/* +0xA2 */ uint8 micStatus;
	/* +0xA3 */ uint8 slideVolume2;
	/* +0xA4 */ uint8 padding4[8];
};

using VPADStatus_t = VPADStatus;
static_assert(sizeof(VPADStatus) == 0xAC);
