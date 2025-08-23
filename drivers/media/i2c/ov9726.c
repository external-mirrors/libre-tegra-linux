// SPDX-License-Identifier: GPL-2.0-only

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/media.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/ratelimit.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/videodev2.h>

#include <media/media-entity.h>
#include <media/v4l2-async.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-event.h>
#include <media/v4l2-image-sizes.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-mediabus.h>

/* product information registers */
#define OV9726_PRODUCT_ID			CCI_REG16(0x0000)
#define   OV9726_CHIP_ID			0x9726
#define OV9726_REVISION				CCI_REG8(0x0002)
#define OV9726_MANUFACTURER_ID			CCI_REG8(0x0003)
#define OV9726_FRAME_COUNTER			CCI_REG8(0x0005)
#define OV9726_PIXEL_ORDER			CCI_REG8(0x0006)

/* general configuration registers */
#define OV9726_STREAMING_MODE			CCI_REG8(0x0100)
#define   OV9726_MODE_STANDBY			0
#define   OV9726_MODE_STREAMING			1
#define OV9726_IMAGE_ORIENTATION		CCI_REG8(0x0101)
#define   OV9726_IMAGE_HFLIP			BIT(0)
#define   OV9726_IMAGE_VFLIP			BIT(1)
#define OV9726_SOFTWARE_RESET			CCI_REG8(0x0103)
#define   OV9726_RESET_ON			1
#define OV9726_GROUP_WRITE			CCI_REG8(0x0104)
#define   OV9726_GROUP_WRITE_ON			1
#define OV9726_FRAME_DROP			CCI_REG8(0x0105)
#define   OV9726_FRAME_DROP_ON			1
#define OV9726_DATA_DEPTH			CCI_REG16(0x0112)
#define   OV9726_DATA_DEPTH_RAW10		0x0a

/* integration time registers */
#define OV9726_INTEGRATION_TIME_LINE		CCI_REG16(0x0202)
#define   OV9726_INTEGRATION_TIME_MIN		0x2
#define   OV9726_INTEGRATION_TIME_MAX		0xffff
#define   OV9726_INTEGRATION_TIME_STEP		1
#define   OV9726_INTEGRATION_TIME_DEF		0x313

/* analog gain registers */
#define OV9726_ANALOG_GAIN			CCI_REG8(0x0205)
#define   OV9726_ANA_GAIN_MIN			0
#define   OV9726_ANA_GAIN_MAX			0xff
#define   OV9726_ANA_GAIN_STEP			1
#define   OV9726_ANA_GAIN_DEFAULT		0x3f

/* auto gain/exposure registers */
#define OV9726_AGC_AEC				CCI_REG8(0x3503)
#define   OV9726_AGC_MASK			BIT(0) /* clear bit for auto */
#define   OV9726_AEC_MASK			BIT(1) /* clear bit for auto */

/* clock configuration registers */
#define OV9726_PIXEL_CLK_DIVIDER_PLL1		CCI_REG8(0x0301)
#define OV9726_SYSTEM_CLK_DIVIDER_PLL1		CCI_REG8(0x0303) /* fixed to 1 */
#define OV9726_PRE_PLL_CLK_DIVIDER_PLL1		CCI_REG8(0x0305)
#define OV9726_PLL_MULTIPLIER_PLL1		CCI_REG8(0x0307)
#define OV9726_SCALE_DIVIDER_PLL1		CCI_REG8(0x3010)
#define OV9726_PRE_PLL_CLK_DIVIDER_PLL2		CCI_REG8(0x300c)
#define OV9726_PLL_MULTIPLIER_PLL2		CCI_REG8(0x300d)
#define OV9726_SYSTEM_CLK_DIVIDER_PLL2		CCI_REG8(0x300e)
#define OV9726_PLL_RST_SW			CCI_REG8(0x3104)

/* frame timing registers */
#define OV9726_VERTICAL_TOTAL_LENGTH		CCI_REG16(0x0340)
#define OV9726_HORIZONTAL_TOTAL_LENGTH		CCI_REG16(0x0342)

/* image size registers */
#define OV9726_HORIZONTAL_START			CCI_REG16(0x0344)
#define OV9726_VERTICAL_START			CCI_REG16(0x0346)
#define OV9726_HORIZONTAL_END			CCI_REG16(0x0348)
#define OV9726_VERTICAL_END			CCI_REG16(0x034a)
#define OV9726_IMAGE_WIDTH			CCI_REG16(0x034c)
#define OV9726_IMAGE_HEIGHT			CCI_REG16(0x034e)

/* test pattern registers */
#define OV9726_TEST_PATTERN			CCI_REG8(0x0601)
#define   OV9726_TEST_PATTERN_NONE		0
#define   OV9726_TEST_PATTERN_SOLID		1
#define   OV9726_TEST_PATTERN_BARS		2
#define   OV9726_TEST_PATTERN_FADE		3
#define OV9726_SOLID_COLOR_RED			CCI_REG16(0x0602)
#define OV9726_SOLID_COLOR_GR			CCI_REG16(0x0604)
#define OV9726_SOLID_COLOR_BLUE			CCI_REG16(0x0606)
#define OV9726_SOLID_COLOR_GB			CCI_REG16(0x0608)
#define   OV9726_TESTP_COLOUR_MIN		0
#define   OV9726_TESTP_COLOUR_MAX		0x03ff
#define   OV9726_TESTP_COLOUR_STEP		1

#define OV9726_PIXEL_ARRAY_WIDTH		1296U
#define OV9726_PIXEL_ARRAY_HEIGHT		808U

enum {
	OV9726_MODE_1280x800,
	OV9726_MODE_1280x720,
//	OV9726_MODE_640x400,
//	OV9726_MODE_320x200
};

struct ov9726_mode {
	u32 width;
	u32 height;
	u32 framerate;
	u32 hts;	/* Horizontal timining size */
	u32 vts;	/* Vertical timining size */
	struct v4l2_rect crop;
};

struct ov9726 {
	struct regmap *regmap;

	struct clk *xvclk;
	struct gpio_desc *reset;
	struct gpio_desc *pwdn;
	struct regulator_bulk_data supplies[3];

	struct v4l2_fwnode_endpoint bus_cfg;
	struct v4l2_subdev sd;
	struct media_pad pad;

	/* V4L2 Controls */
	struct v4l2_ctrl_handler hdl;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *hflip;
	struct v4l2_ctrl *vflip;

	struct {
		u32 pre_div;
		u32 mult;
		u32 pix_clk_div;
	} pll;

	const struct ov9726_mode *cur_mode;

	u64 pixel_clk;
	s64 default_link_freq;
};

static const struct ov9726_mode ov9726_modes[] = {
	[OV9726_MODE_1280x800] = {
		.width		= 1280,
		.height		= 800,
		.framerate	= 30,
		.hts		= 1664,
		.vts		= 840,
		.crop = {
			.top	= 0,
			.left	= 0,
			.width	= 1296,
			.height	= 808,
		},
	},
	[OV9726_MODE_1280x720] = {
		.width		= 1280,
		.height		= 720,
		.framerate	= 30,
		.hts		= 1664,
		.vts		= 840,
		.crop = {
			.top	= 0,
			.left	= 40,
			.width	= 1280,
			.height	= 728,
		},
	},
#if 0
	[OV9726_MODE_640x400] = {
		.width		= 640,
		.height		= 400,
		.framerate	= 90,
		.hts		= 1120,
		.vts		= 520, // 400+120 ??
		.crop = { // ??
			.top	= 0,
			.left	= 0,
			.width	= 0,
			.height	= 0,
		},
	},
	[OV9726_MODE_320x200] = {
		.width		= 320,
		.height		= 200,
		.framerate	= 120,
		.hts		= 840,
		.vts		= 320, // 200+120 ??
		.crop = { // ??
			.top	= 0,
			.left	= 0,
			.width	= 0,
			.height	= 0,
		},
	},
#endif
};

/*
 * This table MUST contain 4 entries per format, to cover the various flip
 * combinations in the order
 * - no flip
 * - h flip
 * - v flip
 * - h&v flips
 */
static const u32 ov9726_mbus_formats[] = {
	MEDIA_BUS_FMT_SBGGR10_1X10,
	MEDIA_BUS_FMT_SGBRG10_1X10,
	MEDIA_BUS_FMT_SGRBG10_1X10,
	MEDIA_BUS_FMT_SRGGB10_1X10,
};

static inline struct ov9726 *sd_to_ov9726(struct v4l2_subdev *sd)
{
	return container_of(sd, struct ov9726, sd);
}

static inline struct ov9726 *ctrl_to_ov9726(struct v4l2_ctrl *ctrl)
{
	return container_of(ctrl->handler, struct ov9726, hdl);
}

static u32 ov9726_get_format_code(struct ov9726 *sensor, u32 code, bool test)
{
	u32 i;

	for (i = 0; i < ARRAY_SIZE(ov9726_mbus_formats); i++)
		if (ov9726_mbus_formats[i] == code)
			break;

	if (i >= ARRAY_SIZE(ov9726_mbus_formats))
		i = 0;

	if (test)
		return ov9726_mbus_formats[i];

	i = (i & ~3) | (sensor->vflip->val ? 2 : 0) |
	    (sensor->hflip->val ? 1 : 0);

	return ov9726_mbus_formats[i];
}

static int ov9726_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ov9726 *sensor = ctrl_to_ov9726(ctrl);
	struct device *dev = regmap_get_device(sensor->regmap);
	int ret = 0;

	/*
	 * Applying V4L2 control value only happens
	 * when power is up for streaming
	 */
	if (!pm_runtime_get_if_in_use(dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_AUTOGAIN:
		cci_update_bits(sensor->regmap, OV9726_AGC_AEC, OV9726_AGC_MASK,
				!ctrl->val, &ret);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		cci_write(sensor->regmap, OV9726_ANALOG_GAIN, ctrl->val, &ret);
		break;
	case V4L2_CID_EXPOSURE_AUTO:
		cci_update_bits(sensor->regmap, OV9726_AGC_AEC, OV9726_AEC_MASK,
				ctrl->val << 1, &ret);
		break;
	case V4L2_CID_EXPOSURE:
		cci_update_bits(sensor->regmap, OV9726_GROUP_WRITE, OV9726_GROUP_WRITE_ON,
				OV9726_GROUP_WRITE_ON, NULL);
		cci_write(sensor->regmap, OV9726_INTEGRATION_TIME_LINE, ctrl->val, NULL);
		cci_update_bits(sensor->regmap, OV9726_GROUP_WRITE, OV9726_GROUP_WRITE_ON,
				0, NULL);
		break;
	case V4L2_CID_HFLIP:
	case V4L2_CID_VFLIP:
		cci_write(sensor->regmap, OV9726_IMAGE_ORIENTATION,
			  sensor->hflip->val | sensor->vflip->val << 1, &ret);
		break;
	case V4L2_CID_TEST_PATTERN:
		cci_write(sensor->regmap, OV9726_TEST_PATTERN, ctrl->val, &ret);
		break;
	case V4L2_CID_TEST_PATTERN_RED:
		cci_update_bits(sensor->regmap, OV9726_GROUP_WRITE, OV9726_GROUP_WRITE_ON,
				OV9726_GROUP_WRITE_ON, NULL);
		cci_write(sensor->regmap, OV9726_SOLID_COLOR_RED, ctrl->val, &ret);
		cci_update_bits(sensor->regmap, OV9726_GROUP_WRITE, OV9726_GROUP_WRITE_ON,
				0, NULL);
		break;
	case V4L2_CID_TEST_PATTERN_GREENR:
		cci_update_bits(sensor->regmap, OV9726_GROUP_WRITE, OV9726_GROUP_WRITE_ON,
				OV9726_GROUP_WRITE_ON, NULL);
		cci_write(sensor->regmap, OV9726_SOLID_COLOR_GR, ctrl->val, &ret);
		cci_update_bits(sensor->regmap, OV9726_GROUP_WRITE, OV9726_GROUP_WRITE_ON,
				0, NULL);
		break;
	case V4L2_CID_TEST_PATTERN_BLUE:
		cci_update_bits(sensor->regmap, OV9726_GROUP_WRITE, OV9726_GROUP_WRITE_ON,
				OV9726_GROUP_WRITE_ON, NULL);
		cci_write(sensor->regmap, OV9726_SOLID_COLOR_BLUE, ctrl->val, &ret);
		cci_update_bits(sensor->regmap, OV9726_GROUP_WRITE, OV9726_GROUP_WRITE_ON,
				0, NULL);
		break;
	case V4L2_CID_TEST_PATTERN_GREENB:
		cci_update_bits(sensor->regmap, OV9726_GROUP_WRITE, OV9726_GROUP_WRITE_ON,
				OV9726_GROUP_WRITE_ON, NULL);
		cci_write(sensor->regmap, OV9726_SOLID_COLOR_GB, ctrl->val, &ret);
		cci_update_bits(sensor->regmap, OV9726_GROUP_WRITE, OV9726_GROUP_WRITE_ON,
				0, NULL);
		break;
	default:
		ret = -EINVAL;
	}

	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);

	return ret;
}

static const struct v4l2_ctrl_ops ov9726_ctrl_ops = {
	.s_ctrl = ov9726_set_ctrl,
};

static const char * const test_pattern_menu[] = {
	"Disabled",
	"Solid Color Fill",
	"Standard Color Bars",
	"Fade To Grey Color Bars",
};

static int ov9726_init_controls(struct ov9726 *sensor)
{
	const struct v4l2_ctrl_ops *ops = &ov9726_ctrl_ops;
	struct device *dev = regmap_get_device(sensor->regmap);
	struct v4l2_fwnode_device_properties props;
	struct v4l2_subdev *sd = &sensor->sd;
	struct v4l2_ctrl_handler *hdl = &sensor->hdl;
	int i, ret;

	ret = v4l2_fwnode_device_parse(dev, &props);
	if (ret < 0)
		return ret;

	ret = v4l2_ctrl_handler_init(hdl, 13);
	if (ret)
		return ret;

	sensor->pixel_rate = v4l2_ctrl_new_std(hdl, NULL, V4L2_CID_PIXEL_RATE, 0,
					       sensor->pixel_clk, 1,
					       sensor->pixel_clk);

	sensor->link_freq = v4l2_ctrl_new_int_menu(hdl, NULL, V4L2_CID_LINK_FREQ,
						   0, 0, &sensor->default_link_freq);
	if (sensor->link_freq)
		sensor->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_AUTOGAIN, 0, 1, 1, 1);
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_ANALOGUE_GAIN,
			  OV9726_ANA_GAIN_MIN, OV9726_ANA_GAIN_MAX,
			  OV9726_ANA_GAIN_STEP, OV9726_ANA_GAIN_DEFAULT);

	sensor->hflip = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_HFLIP, 0, 1, 1, 0);
	if (sensor->hflip)
		sensor->hflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	sensor->vflip = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_VFLIP, 0, 1, 1, 0);
	if (sensor->vflip)
		sensor->vflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	/*
	 * The maximum coarse integration time is the frame length in lines
	 * minus eight.
	 */
	sensor->exposure = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_EXPOSURE,
					     OV9726_INTEGRATION_TIME_MIN,
					     OV9726_PIXEL_ARRAY_HEIGHT - 8,
					     OV9726_INTEGRATION_TIME_STEP,
					     OV9726_PIXEL_ARRAY_HEIGHT - 8);
	v4l2_ctrl_new_std_menu(hdl, ops, V4L2_CID_EXPOSURE_AUTO,
			       V4L2_EXPOSURE_MANUAL, 0, V4L2_EXPOSURE_AUTO);

	v4l2_ctrl_new_fwnode_properties(hdl, ops, &props);

	v4l2_ctrl_new_std_menu_items(hdl, ops, V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(test_pattern_menu) - 1, 0, 0,
				     test_pattern_menu);

	for (i = 0; i < 4; i++) {
		/*
		 * The assumption is that
		 * V4L2_CID_TEST_PATTERN_GREENR == V4L2_CID_TEST_PATTERN_RED + 1
		 * V4L2_CID_TEST_PATTERN_BLUE   == V4L2_CID_TEST_PATTERN_RED + 2
		 * V4L2_CID_TEST_PATTERN_GREENB == V4L2_CID_TEST_PATTERN_RED + 3
		 */
		v4l2_ctrl_new_std(hdl, ops, V4L2_CID_TEST_PATTERN_RED + i,
				  OV9726_TESTP_COLOUR_MIN, OV9726_TESTP_COLOUR_MAX,
				  OV9726_TESTP_COLOUR_STEP, OV9726_TESTP_COLOUR_MAX);
		/* The "Solid color" pattern is white by default */
	}

	if (hdl->error)
		return hdl->error;

	sd->ctrl_handler = hdl;

	return 0;
};

// TODO: refine and remove
// WXGA(1280 * 800) - 30fps - hts = 1664 * pclk
// 640 * 400 - 90fps - hts = 1120 * pclk
// 320 * 200 - 120fps - hts = 840 * pclk
static const struct cci_reg_sequence ov9726_init_first[] = {
	{ CCI_REG8(0x3026), 0x00 }, /* OUTPUT_SELECT01 */
	{ CCI_REG8(0x3027), 0x00 }, /* OUTPUT_SELECT02 */

	{ CCI_REG8(0x3705), 0x45 }, //SENSOR REG05 - Sensor Timing Control
	{ CCI_REG8(0x3603), 0xaa }, //ANA ADC03 - Analog Control
	{ CCI_REG8(0x3632), 0x2f }, //ANA PWC02 - Analog Control
	{ CCI_REG8(0x3620), 0x66 }, //ANA ARRAY00
	{ CCI_REG8(0x3621), 0xc0 }, //ANA ARRAY01

//	{ CCI_REG8(0x0202), 0x03 }, /* COARSE_INTEGRATION_TIME */
//	{ CCI_REG8(0x0203), 0x13 }, /* COARSE_INTEGRATION_TIME_LO */

	{ CCI_REG8(0x3833), 0x04 }, //ISP X WIN
	{ CCI_REG8(0x3835), 0x02 }, //ISP Y WIN

	{ CCI_REG8(0x4702), 0x04 }, //DVP_HSYVSY_NEG_WIDTH
	{ CCI_REG8(0x4704), 0x00 }, /* DVP_CTRL01 */
	{ CCI_REG8(0x4706), 0x08 }, // DVP_EOF_VSYNC_DELAY
	{ CCI_REG8(0x5052), 0x01 }, //ISP CTRL52
	{ CCI_REG8(0x3819), 0x6c }, //TIMING TC REG19
	{ CCI_REG8(0x3817), 0x94 }, //TIMING TC REG17
	{ CCI_REG8(0x404e), 0x7e }, //BLC CTRL4E - Max black level
	{ CCI_REG8(0x3601), 0x40 }, //ANA ADC01
	{ CCI_REG8(0x3610), 0xa0 }, //ANA ANALOG00
};

#if 0
	{ CCI_REG8(0x0344), 0x00 }, //HORIZONTAL_START hi
	{ CCI_REG8(0x0345), 0x00 }, //HORIZONTAL_START lo
	{ CCI_REG8(0x0346), 0x00 }, // 0x0028 for 1280x720 or 0x0000 for 1280x800
	{ CCI_REG8(0x0347), 0x28 }, //VERTICAL_START lo
	{ CCI_REG8(0x034c), 0x05 }, /* IMAGE_WIDTH */ // 0x0500 = 1280 or 0x0510 = 1296(full) for 1280x800
	{ CCI_REG8(0x034d), 0x00 }, /* IMAGE_WIDTH_LO */
	{ CCI_REG8(0x034e), 0x02 }, /* IMAGE_HEIGHT */ // 0x02d8 = 720+8 or 0x0328 = 800+8 for 1280x800
	{ CCI_REG8(0x034f), 0xd8 }, /* IMAGE_HEIGHT_LO */
#endif

static const struct cci_reg_sequence ov9726_init_second[] = {
	{ CCI_REG8(0x3002), 0x00 }, /* IO_CTRL00 */
	{ CCI_REG8(0x3004), 0x00 }, /* IO_CTRL01 */
	{ CCI_REG8(0x3005), 0x00 }, /* IO_CTRL02 */

	{ CCI_REG8(0x4800), 0x44 }, // MIPI CTRL 00
	{ CCI_REG8(0x4801), 0x0f }, /* MIPI_CTRL01 */
	{ CCI_REG8(0x4803), 0x05 }, /* MIPI_CTRL03 */

	{ CCI_REG8(0x4601), 0x16 }, /* VFIFO_READ_CONTROL */
	{ CCI_REG8(0x3014), 0x05 }, /* SC_CMMN_MIPI / SC_CTRL00 */

//	{ OV9726_IMAGE_ORIENTATION, 0x01 },

	{ CCI_REG8(0x3707), 0x14 }, //SENSOR REG07
	{ CCI_REG8(0x3622), 0x9f }, //ANA ARRAY02
	{ CCI_REG8(0x4002), 0x45 }, /* BLC_CTRL02 */
	{ CCI_REG8(0x5001), 0x00 }, /* ISP_CTRL1 */
	{ CCI_REG8(0x3406), 0x01 }, /* AWB_MANUAL_CTRL */

	{ CCI_REG8(0x3503), 0x17 }, /* AEC_ENABLE */

//	{ OV9726_ANALOG_GAIN, 0x3f },
};

#if 0
	{ OV9726_STREAMING_MODE, 0x01 }, // MODE_SELECT - streaming
	{ CCI_REG8(0x0112), 0x0a }, //DATA DEPTH hi - RAW10
	{ CCI_REG8(0x0113), 0x0a }, //DATA DEPTH lo - RAW10

	{ CCI_REG8(0x3013), 0x20 },
	{ CCI_REG8(0x4837), 0x2f }, //MIPI PCLK PERIOD
	{ CCI_REG8(0x3615), 0xf0 }, //ANA ANALOG05

	{ CCI_REG8(0x0340), 0x03 }, /* VERTICAL_TOTAL_LENGTH */ // frame length: 840  (reg 0x340/0x341)
	{ CCI_REG8(0x0341), 0x48 }, /* VERTICAL_TOTAL_LENGTH_LO */
	{ CCI_REG8(0x0342), 0x06 }, /* HORIZONTAL_TOTAL_LENGTH */ // line length:  1664 (reg 0x342/0x343)
	{ CCI_REG8(0x0343), 0x80 }, /* HORIZONTAL_TOTAL_LENGTH_LO */

	{ CCI_REG8(0x3702), 0x1e }, //SENSOR RSTGOLOW
	{ CCI_REG8(0x3703), 0x3c }, //SENSOR HLDWIDTH
	{ CCI_REG8(0x3704), 0x0e }, //SENSOR TXWIDTH

	{ OV9726_PLL_RST_SW, 0x20 },  //PLL_RST_SW - 0x20 is reset

	{ OV9726_PRE_PLL_CLK_DIVIDER_PLL1, 0x04 }, // pre_pll_clk_div
	{ OV9726_PLL_MULTIPLIER_PLL1, 0x46 }, // pll_multiplier
	{ OV9726_SYSTEM_CLK_DIVIDER_PLL1, 0x01 }, // vt_sys_clk_div
	{ OV9726_PIXEL_CLK_DIVIDER_PLL1, 0x0a },
	{ OV9726_SCALE_DIVIDER_PLL1, 0x01 }, // divmip
#endif

static const struct cci_reg_sequence ov9726_init_third[] = {
	{ CCI_REG8(0x460e), 0x00 }, /* VFIFO_CONTROL00 */

	{ CCI_REG8(0x5000), 0x00 }, // ISP_CTRL0  - disable all
	{ CCI_REG8(0x5002), 0x00 }, //ISP CTRL02 - disable all

	{ CCI_REG8(0x3017), 0xd2 }, //SC CMMN CLKRST01 - parametrize
	{ CCI_REG8(0x3018), 0x69 }, //SC CMMN CLKRST02 - parametrize
	{ CCI_REG8(0x3019), 0x96 }, //SC CMMN CLKRST03 - parametrize

	{ CCI_REG8(0x5047), 0x61 }, // ISP_CTRL47  - parametrize
	{ CCI_REG8(0x3604), 0x1c }, //ANA ADC04
	{ CCI_REG8(0x3602), 0x10 }, //ANA ADC02
	{ CCI_REG8(0x3612), 0x21 }, //ANA ANALOG02
	{ CCI_REG8(0x3630), 0x0a }, //ANA PWC00
	{ CCI_REG8(0x3631), 0x53 }, //ANA PWC01
	{ CCI_REG8(0x3633), 0x70 }, //ANA PWC03
	{ CCI_REG8(0x4005), 0x1a }, // BLC_CTRL05 - Output black line disable
	{ CCI_REG8(0x4009), 0x10 }, //BLC CTRL09
};

static int ov9726_start_streaming(struct ov9726 *sensor)
{
	struct device *dev = regmap_get_device(sensor->regmap);
	const struct ov9726_mode *mode = sensor->cur_mode;
	int ret;

	cci_write(sensor->regmap, OV9726_SOFTWARE_RESET, OV9726_RESET_ON, &ret);
	usleep_range(10000, 11000);

	ret = cci_multi_reg_write(sensor->regmap, ov9726_init_first,
				  ARRAY_SIZE(ov9726_init_first), NULL);
	if (ret < 0) {
		dev_err(dev, "Failed to initialize the sensor\n");
		return ret;
	}

	cci_update_bits(sensor->regmap, OV9726_GROUP_WRITE, OV9726_GROUP_WRITE_ON,
			OV9726_GROUP_WRITE_ON, &ret);

	cci_write(sensor->regmap, OV9726_HORIZONTAL_START, mode->crop.top, &ret);
	cci_write(sensor->regmap, OV9726_VERTICAL_START, mode->crop.left, &ret);
	cci_write(sensor->regmap, OV9726_IMAGE_WIDTH, mode->crop.width, &ret);
	cci_write(sensor->regmap, OV9726_IMAGE_HEIGHT, mode->crop.height, &ret);

	cci_update_bits(sensor->regmap, OV9726_GROUP_WRITE, OV9726_GROUP_WRITE_ON,
			0, &ret);

	ret = cci_multi_reg_write(sensor->regmap, ov9726_init_second,
				  ARRAY_SIZE(ov9726_init_second), NULL);
	if (ret < 0) {
		dev_err(dev, "Failed to initialize the sensor\n");
		return ret;
	}

	ret = __v4l2_ctrl_handler_setup(&sensor->hdl);
	if (ret)
		return ret;

	ret = cci_write(sensor->regmap, OV9726_STREAMING_MODE, OV9726_MODE_STREAMING, NULL);
	if (ret) {
		dev_err(dev, "failed to start stream");
		return ret;
	}

	cci_update_bits(sensor->regmap, OV9726_GROUP_WRITE, OV9726_GROUP_WRITE_ON,
			OV9726_GROUP_WRITE_ON, &ret);

	cci_write(sensor->regmap, OV9726_DATA_DEPTH,
		  OV9726_DATA_DEPTH_RAW10 | OV9726_DATA_DEPTH_RAW10 << 8, &ret);

	cci_update_bits(sensor->regmap, OV9726_GROUP_WRITE, OV9726_GROUP_WRITE_ON,
			0, &ret);

	cci_write(sensor->regmap, CCI_REG8(0x3013), 0x20, &ret);
	cci_write(sensor->regmap, CCI_REG8(0x4837), 0x2f, &ret); //MIPI PCLK PERIOD
	cci_write(sensor->regmap, CCI_REG8(0x3615), 0xf0, &ret); //ANA ANALOG05

	cci_update_bits(sensor->regmap, OV9726_GROUP_WRITE, OV9726_GROUP_WRITE_ON,
			OV9726_GROUP_WRITE_ON, &ret);

	cci_write(sensor->regmap, OV9726_VERTICAL_TOTAL_LENGTH, mode->vts, &ret);
	cci_write(sensor->regmap, OV9726_HORIZONTAL_TOTAL_LENGTH, mode->hts, &ret);

	cci_update_bits(sensor->regmap, OV9726_GROUP_WRITE, OV9726_GROUP_WRITE_ON,
			0, &ret);
 
	cci_write(sensor->regmap, CCI_REG8(0x3702), 0x1e, &ret); //SENSOR RSTGOLOW
	cci_write(sensor->regmap, CCI_REG8(0x3703), 0x3c, &ret); //SENSOR HLDWIDTH
	cci_write(sensor->regmap, CCI_REG8(0x3704), 0x0e, &ret); //SENSOR TXWIDTH

	cci_write(sensor->regmap, OV9726_PLL_RST_SW, 0x20, &ret); //PLL_RST_SW - 0x20 is reset

	cci_write(sensor->regmap, OV9726_PRE_PLL_CLK_DIVIDER_PLL1, sensor->pll.pre_div, &ret);
	cci_write(sensor->regmap, OV9726_PLL_MULTIPLIER_PLL1, sensor->pll.mult, &ret);
	cci_write(sensor->regmap, OV9726_SYSTEM_CLK_DIVIDER_PLL1, 1, &ret);
	cci_write(sensor->regmap, OV9726_PIXEL_CLK_DIVIDER_PLL1, sensor->pll.pix_clk_div, &ret);
	cci_write(sensor->regmap, OV9726_SCALE_DIVIDER_PLL1, 1, &ret);

	ret = cci_multi_reg_write(sensor->regmap, ov9726_init_third,
				  ARRAY_SIZE(ov9726_init_third), NULL);
	if (ret < 0) {
		dev_err(dev, "Failed to initialize the sensor\n");
		return ret;
	}

	/* vflip and hflip cannot change during streaming */
	__v4l2_ctrl_grab(sensor->vflip, true);
	__v4l2_ctrl_grab(sensor->hflip, true);

	return ret;
}

static void ov9726_stop_streaming(struct ov9726 *sensor)
{
	struct device *dev = regmap_get_device(sensor->regmap);

	if (cci_write(sensor->regmap, OV9726_STREAMING_MODE, OV9726_MODE_STANDBY, NULL))
		dev_err(dev, "failed to stop stream");

	__v4l2_ctrl_grab(sensor->vflip, false);
	__v4l2_ctrl_grab(sensor->hflip, false);
}

static int ov9726_set_stream(struct v4l2_subdev *sd, int enable)
{
	struct ov9726 *sensor = sd_to_ov9726(sd);
	struct device *dev = regmap_get_device(sensor->regmap);
	struct v4l2_subdev_state *state;
	int ret = 0;

	state = v4l2_subdev_lock_and_get_active_state(sd);

	if (enable) {
		ret = pm_runtime_resume_and_get(dev);
		if (ret)
			goto finish_unlock;

		ret = ov9726_start_streaming(sensor);
		if (!ret)
			goto finish_unlock;

		dev_err(dev, "Failed to start stream: %d\n", ret);
		enable = 0;
	}

	ov9726_stop_streaming(sensor);
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);

finish_unlock:
	v4l2_subdev_unlock_state(state);

	return ret;
}

/* -----------------------------------------------------------------------------
 * OV9726 Pad Subdev Init and Operations
 */
static int ov9726_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	struct ov9726 *sensor = sd_to_ov9726(sd);

	if (code->index > 0)
		return -EINVAL;

	code->code = ov9726_get_format_code(sensor, ov9726_mbus_formats[code->index], false);

	return 0;
}

static int ov9726_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	struct ov9726 *sensor = sd_to_ov9726(sd);
	u32 code;

	if (fse->index >= ARRAY_SIZE(ov9726_modes))
		return -EINVAL;

	code = ov9726_get_format_code(sensor, fse->code, true);
	if (fse->code != code)
		return -EINVAL;

	fse->min_width = ov9726_modes[fse->index].width;
	fse->max_width = fse->min_width;
	fse->min_height = ov9726_modes[fse->index].height;
	fse->max_height = fse->max_height;

	return 0;
}

static int ov9726_enum_frame_interval(struct v4l2_subdev *sd,
				      struct v4l2_subdev_state *state,
				      struct v4l2_subdev_frame_interval_enum *fie)
{
	const struct ov9726_mode *mode;
	struct ov9726 *sensor = sd_to_ov9726(sd);
	u32 code;

	if (fie->index > 0)
		return -EINVAL;

	code = ov9726_get_format_code(sensor, fie->code, true);
	if (fie->code != code)
		return -EINVAL;

	mode = v4l2_find_nearest_size(ov9726_modes, ARRAY_SIZE(ov9726_modes),
				      width, height,
				      fie->width, fie->height);
	if (fie->width > mode->width || fie->height > mode->height)
		return -EINVAL;

	fie->interval.numerator = 1;
	fie->interval.denominator = mode->framerate;

	return 0;
}

static int ov9726_set_format(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state,
			     struct v4l2_subdev_format *format)
{
	struct ov9726 *sensor = sd_to_ov9726(sd);
	struct v4l2_mbus_framefmt *mbus_fmt = &format->format;
	struct v4l2_mbus_framefmt *fmt;
	struct v4l2_fract *interval;
	const struct ov9726_mode *mode;

	mode = v4l2_find_nearest_size(ov9726_modes, ARRAY_SIZE(ov9726_modes),
				      width, height,
				      mbus_fmt->width, mbus_fmt->height);

	fmt = v4l2_subdev_state_get_format(state, format->pad);

	fmt->code = ov9726_get_format_code(sensor, mbus_fmt->code, false);
	fmt->width = mode->width;
	fmt->height = mode->height;
	fmt->colorspace = V4L2_COLORSPACE_SRGB;

	*mbus_fmt = *fmt;

	interval = v4l2_subdev_state_get_interval(state, format->pad);
	interval->numerator = 1;
	interval->denominator = mode->framerate;

	if (format->which == V4L2_SUBDEV_FORMAT_ACTIVE)
		sensor->cur_mode = mode;

	return 0;
}

static int ov9726_set_frame_interval(struct v4l2_subdev *sd,
				     struct v4l2_subdev_state *state,
				     struct v4l2_subdev_frame_interval *fi)
{
	const struct v4l2_mbus_framefmt *mbus_fmt;
	const struct ov9726_mode *mode;
	struct v4l2_fract *interval;

	mbus_fmt = v4l2_subdev_state_get_format(state, fi->pad);

	mode = v4l2_find_nearest_size(ov9726_modes, ARRAY_SIZE(ov9726_modes),
				      width, height,
				      mbus_fmt->width, mbus_fmt->height);

	interval = v4l2_subdev_state_get_interval(state, fi->pad);

	interval->numerator = 1;
	interval->denominator = mode->framerate;

	fi->interval = *interval;

	return 0;
}

static int ov9726_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *sd_state)
{
	struct ov9726 *sensor = sd_to_ov9726(sd);
	const struct ov9726_mode *mode = sensor->cur_mode;
	struct v4l2_mbus_framefmt *fmt;
	struct v4l2_fract *interval;

	fmt = v4l2_subdev_state_get_format(sd_state, 0);
	interval = v4l2_subdev_state_get_interval(sd_state, 0);

	fmt->code = MEDIA_BUS_FMT_SBGGR10_1X10;
	fmt->width = mode->width;
	fmt->height = mode->height;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_SRGB;
	fmt->ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(fmt->colorspace);
	fmt->quantization = V4L2_MAP_QUANTIZATION_DEFAULT(true,
							  fmt->colorspace,
							  fmt->ycbcr_enc);
	fmt->xfer_func = V4L2_MAP_XFER_FUNC_DEFAULT(fmt->colorspace);

	interval->numerator = 1;
	interval->denominator = mode->framerate;

	return 0;
}

static const struct v4l2_subdev_video_ops ov9726_video_ops = {
	.s_stream = ov9726_set_stream,
};

static const struct v4l2_subdev_pad_ops ov9726_pad_ops = {
	.enum_mbus_code = ov9726_enum_mbus_code,
	.enum_frame_size = ov9726_enum_frame_size,
	.enum_frame_interval = ov9726_enum_frame_interval,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = ov9726_set_format,
	.get_frame_interval = v4l2_subdev_get_frame_interval,
	.set_frame_interval = ov9726_set_frame_interval,
};

static const struct v4l2_subdev_ops ov9726_subdev_ops = {
	.video = &ov9726_video_ops,
	.pad = &ov9726_pad_ops,
};

static const struct media_entity_operations ov9726_subdev_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static const struct v4l2_subdev_internal_ops ov9726_internal_ops = {
	.init_state = ov9726_init_state,
};

static int ov9726_init_subdev(struct ov9726 *sensor, struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct v4l2_subdev *sd = &sensor->sd;
	struct media_pad *pad = &sensor->pad;
	struct v4l2_ctrl_handler *hdl = &sensor->hdl;
	int ret;

	/* Initialize the subdev. */
	v4l2_i2c_subdev_init(sd, client, &ov9726_subdev_ops);

	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sd->internal_ops = &ov9726_internal_ops;

	/* Initialize the media entity. */
	sd->entity.function = MEDIA_ENT_F_CAM_SENSOR;
	sd->entity.ops = &ov9726_subdev_entity_ops;
	pad->flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&sd->entity, 1, pad);
	if (ret < 0) {
		dev_err(dev, "failed to init entity pads: %d", ret);
		return ret;
	}

	/* Initialize the control handler. */
	ret = ov9726_init_controls(sensor);
	if (ret)
		goto error;

	return 0;
error:
	v4l2_ctrl_handler_free(hdl);
	media_entity_cleanup(&sd->entity);
	return ret;
};

/* -----------------------------------------------------------------------------
 * Power Management
 */

static int ov9726_power_on(struct ov9726 *sensor)
{
	int ret;

	gpiod_set_value(sensor->reset, 1);
	usleep_range(100, 200);

	ret = regulator_bulk_enable(ARRAY_SIZE(sensor->supplies),
				    sensor->supplies);
	if (ret < 0)
		return ret;

	ret = clk_prepare_enable(sensor->xvclk);
	if (ret < 0)
		goto error_regulator;

	usleep_range(30000, 40000);

	if (sensor->pwdn) {
		gpiod_set_value(sensor->pwdn, 0);
		usleep_range(5000, 6000);
	}

	gpiod_set_value(sensor->reset, 0);
	msleep(20);

	return 0;

error_regulator:
	regulator_bulk_disable(ARRAY_SIZE(sensor->supplies), sensor->supplies);
	return ret;
}

static void ov9726_power_off(struct ov9726 *sensor)
{
	gpiod_set_value(sensor->reset, 1);
	usleep_range(1000, 2000);

	if (sensor->pwdn) {
		gpiod_set_value(sensor->pwdn, 1);
		usleep_range(1000, 1000);
	}

	clk_disable_unprepare(sensor->xvclk);
	regulator_bulk_disable(ARRAY_SIZE(sensor->supplies), sensor->supplies);
}

static int __maybe_unused ov9726_pm_runtime_resume(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct ov9726 *sensor = sd_to_ov9726(sd);

	return ov9726_power_on(sensor);
}

static int __maybe_unused ov9726_pm_runtime_suspend(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct ov9726 *sensor = sd_to_ov9726(sd);

	ov9726_power_off(sensor);

	return 0;
}

static const struct dev_pm_ops ov9726_pm_ops = {
	SET_RUNTIME_PM_OPS(ov9726_pm_runtime_suspend,
			   ov9726_pm_runtime_resume, NULL)
};

/* -----------------------------------------------------------------------------
 * Probe & Remove
 */

static int ov9726_identify_module(struct ov9726 *sensor)
{
	struct device *dev = regmap_get_device(sensor->regmap);
	u64 value, revision, manufacturer;
	int ret;

	ret = cci_read(sensor->regmap, OV9726_PRODUCT_ID, &value, NULL);
	if (ret)
		return ret;

	if (value != OV9726_CHIP_ID) {
		dev_err(dev, "chip id mismatch: %x!=%04llx", OV9726_CHIP_ID, value);
		return -ENXIO;
	}

	cci_read(sensor->regmap, OV9726_REVISION, &revision, NULL);
	cci_read(sensor->regmap, OV9726_MANUFACTURER_ID, &manufacturer, NULL);

	// TODO: make dbg
	dev_warn(dev, "module ov%04llx rev. %llu manufacturer %llu\n",
		 value, revision, manufacturer);

	return 0;
}

static int ov9726_clk_init(struct ov9726 *sensor)
{
	struct device *dev = regmap_get_device(sensor->regmap);
	u64 xvclk_rate, system_clk;
	
	xvclk_rate = clk_get_rate(sensor->xvclk);
	if (!xvclk_rate)
		return dev_err_probe(dev, -EINVAL, "EXTCLK rate unknown\n");

	/* PLL configuration is hardcoded for 24MHz input clock */
	sensor->pll.pre_div = 4;
	sensor->pll.mult = 100;
	sensor->pll.pix_clk_div = 10;

	system_clk = div_u64(xvclk_rate, sensor->pll.pre_div) * sensor->pll.mult;
	sensor->pixel_clk = div_u64(system_clk, sensor->pll.pix_clk_div);

	/*
	 * Calculate the pixel rate and link frequency. The CSI-2 bus is clocked
	 * for 16-bit per pixel, transmitted in DDR over a single lane.
	 */
	sensor->default_link_freq = sensor->pixel_clk * 8;

	if (sensor->bus_cfg.nr_of_link_frequencies != 1 ||
	    sensor->bus_cfg.link_frequencies[0] != sensor->default_link_freq)
		return dev_err_probe(dev, -EINVAL,
				     "Unsupported DT link-frequencies, expected %llu\n",
				     sensor->default_link_freq);

	return 0;
}

static int ov9726_parse_dt(struct ov9726 *sensor)
{
	struct device *dev = regmap_get_device(sensor->regmap);
	struct fwnode_handle *fwnode = dev_fwnode(dev);
	struct fwnode_handle *ep;
	int ret;

	ep = fwnode_graph_get_next_endpoint(fwnode, NULL);
	if (!ep) {
		dev_err(dev, "No endpoint found\n");
		return -EINVAL;
	}

	sensor->bus_cfg.bus_type = V4L2_MBUS_UNKNOWN;
	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &sensor->bus_cfg);
	fwnode_handle_put(ep);
	if (ret < 0) {
		dev_err(dev, "Failed to parse endpoint\n");
		goto error;
	}

	switch (sensor->bus_cfg.bus_type) {
	case V4L2_MBUS_CSI2_DPHY: /* Only CSI2 is supported for now */
		if (sensor->bus_cfg.bus.mipi_csi2.num_data_lanes > 1) {
			dev_err(dev, "number of lanes is more than 1\n");
			ret = -EINVAL;
			goto error;
		}

		break;
	default:
		dev_err(dev, "unsupported bus type %u\n", sensor->bus_cfg.bus_type);
		ret = -EINVAL;
		goto error;
	}

	return 0;

error:
	v4l2_fwnode_endpoint_free(&sensor->bus_cfg);
	return ret;
}

static int ov9726_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct ov9726 *sensor;
	int ret;

	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;

	sensor->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(sensor->regmap))
		return dev_err_probe(dev, PTR_ERR(sensor->regmap),
				     "Failed to allocate register map\n");

	sensor->xvclk = devm_clk_get(dev, NULL);
	if (IS_ERR(sensor->xvclk))
		return dev_err_probe(dev, PTR_ERR(sensor->xvclk), "Failed to get clock\n");

	sensor->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(sensor->reset))
		return dev_err_probe(dev, PTR_ERR(sensor->reset), "Failed to get reset GPIO\n");

	sensor->pwdn = devm_gpiod_get_optional(dev, "powerdown", GPIOD_OUT_HIGH);
	if (IS_ERR(sensor->pwdn))
		return dev_err_probe(dev, PTR_ERR(sensor->pwdn), "Failed to get powerdown GPIO\n");

	sensor->supplies[0].supply = "evdd";
	sensor->supplies[1].supply = "dovdd";
	sensor->supplies[2].supply = "avdd";

	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(sensor->supplies),
				      sensor->supplies);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to get regulators\n");

	ret = ov9726_parse_dt(sensor);
	if (ret < 0)
		return ret;

	ret = ov9726_clk_init(sensor);
	if (ret < 0)
		goto error_ep_free;

	ret = ov9726_power_on(sensor);
	if (ret < 0) {
		dev_err_probe(dev, ret, "Could not power on the device\n");
		goto error_ep_free;
	}

	ret = ov9726_identify_module(sensor);
	if (ret < 0) {
		dev_err_probe(dev, ret, "Could not identify module\n");
		goto error_power_off;
	}

	sensor->cur_mode = &ov9726_modes[OV9726_MODE_1280x800];

	ret = ov9726_init_subdev(sensor, client);
	if (ret < 0) {
		dev_err(dev, "failed to init controls: %d", ret);
		goto error_v4l2_ctrl_handler_free;
	}

	ret = v4l2_subdev_init_finalize(&sensor->sd);
	if (ret)
		goto error_v4l2_ctrl_handler_free;

	/*
	 * Enable runtime PM with autosuspend. As the device has been powered
	 * manually, mark it as active, and increase the usage count without
	 * resuming the device.
	 */
	pm_runtime_set_active(dev);
	pm_runtime_get_noresume(dev);
	pm_runtime_enable(dev);
	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);

	ret = v4l2_async_register_subdev(&sensor->sd);
	if (ret < 0) {
		dev_err(dev, "failed to register V4L2 subdev: %d", ret);
		goto error_pm;
	}

	/*
	 * Decrease the PM usage count. The device will get suspended after the
	 * autosuspend delay, turning the power off.
	 */
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);

	return 0;

error_pm:
	pm_runtime_disable(dev);
	pm_runtime_put_noidle(dev);
	v4l2_subdev_cleanup(&sensor->sd);

error_v4l2_ctrl_handler_free:
	v4l2_ctrl_handler_free(&sensor->hdl);
	media_entity_cleanup(&sensor->sd.entity);

error_power_off:
	ov9726_power_off(sensor);

error_ep_free:
	v4l2_fwnode_endpoint_free(&sensor->bus_cfg);

	return ret;
}

static void ov9726_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov9726 *sensor = sd_to_ov9726(sd);

	v4l2_async_unregister_subdev(&sensor->sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sensor->sd.entity);
	v4l2_ctrl_handler_free(&sensor->hdl);
	v4l2_fwnode_endpoint_free(&sensor->bus_cfg);

	/*
	 * Disable runtime PM. In case runtime PM is disabled in the kernel,
	 * make sure to turn power off manually.
	 */
	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev)) {
		ov9726_power_off(sensor);
		pm_runtime_set_suspended(&client->dev);
	}
}

static const struct i2c_device_id ov9726_id[] = {
	{ "OV9726" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(i2c, ov9726_id);

static const struct of_device_id ov9726_of_match[] = {
	{ .compatible = "ovti,ov9726" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ov9726_of_match);

static struct i2c_driver ov9726_i2c_driver = {
	.driver = {
		.name = "ov9726",
		.of_match_table = ov9726_of_match,
		.pm = &ov9726_pm_ops,
	},
	.id_table = ov9726_id,
	.probe = ov9726_probe,
	.remove = ov9726_remove,
};
module_i2c_driver(ov9726_i2c_driver);

MODULE_AUTHOR("Svyatoslav Ryhel <clamor95@gmail.com>");
MODULE_AUTHOR("Jonas Schwöbel <jonasschwoebel@yahoo.de>");
MODULE_DESCRIPTION("OV9726 CMOS Image Sensor driver");
MODULE_LICENSE("GPL");
