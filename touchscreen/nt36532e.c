// SPDX-License-Identifier: GPL-2.0-only
/*
 * Novatek NT36532E no-flash SPI touchscreen/pen driver.
 * Standalone mainline-style port; no Oplus touchpanel framework dependency.
 */
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>

#define NVT_MAX_TOUCHES 10
#define NVT_POINT_DATA_LEN 120
#define NVT_POINT_CSUM_INDEX 65
#define NVT_PEN_DATA_OFFSET 66
#define NVT_PEN_DATA_LEN 14
#define NVT_XFER_LEN 4096
#define NVT_MAX_PARTITIONS 64
#define NVT_SECTOR_SIZE 4096

#define NVT_EVENT_BUF_ADDR 0x125800
#define NVT_CHIP_VER_TRIM_ADDR 0x1fb104
#define NVT_SWRST_SIF_ADDR 0x1fb43e
#define NVT_ENB_CASC_ADDR 0x1fb12c
#define NVT_BOOT_RDY_ADDR 0x1fb50d
#define NVT_TX_AUTO_COPY_EN 0x1fc925
#define NVT_ILM_LENGTH_ADDR 0x1fb518
#define NVT_DLM_LENGTH_ADDR 0x1fb530
#define NVT_ILM_DES_ADDR 0x1fb528
#define NVT_DLM_DES_ADDR 0x1fb52c
#define NVT_G_ILM_CSUM_ADDR 0x1fb500
#define NVT_G_DLM_CSUM_ADDR 0x1fb504
#define NVT_EVENT_HOST_CMD 0x50
#define NVT_EVENT_RESET_COMPLETE 0x60
#define NVT_RESET_STATE_INIT 0xa0
#define NVT_RESET_STATE_MAX 0xaf
#define NVT_CMD_SLEEP 0x11

struct nvt_partition { u32 bin, sram, size, crc; };

struct nt36532e {
	struct spi_device *spi;
	struct input_dev *input, *pen;
	struct touchscreen_properties prop;
	struct mutex lock;
	const char *fw_name;
	u32 max_x, max_y, max_pressure;
	bool pen_support, high_res, cascade;
	u8 *tx, *rx;
	size_t xfer_size;
};

static u32 nvt_le32(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static int nvt_spi_write(struct nt36532e *ts, const u8 *src, size_t len)
{
	if (!len || len > ts->xfer_size)
		return -EINVAL;
	memcpy(ts->tx, src, len);
	ts->tx[0] |= 0x80;
	return spi_write(ts->spi, ts->tx, len);
}

static int nvt_spi_read(struct nt36532e *ts, u8 *buf, size_t len)
{
	struct spi_transfer xfer = { };
	int ret;

	if (len < 2 || len + 1 > ts->xfer_size)
		return -EINVAL;
	memset(ts->tx, 0, len + 1);
	memset(ts->rx, 0, len + 1);
	memcpy(ts->tx, buf, len);
	ts->tx[0] &= 0x7f;
	xfer.tx_buf = ts->tx;
	xfer.rx_buf = ts->rx;
	xfer.len = len + 1;
	ret = spi_sync_transfer(ts->spi, &xfer, 1);
	if (!ret)
		memcpy(buf + 1, ts->rx + 2, len - 1);
	return ret;
}

static int nvt_set_page(struct nt36532e *ts, u32 addr)
{
	u8 b[3] = { 0xff, (addr >> 15) & 0xff, (addr >> 7) & 0xff };
	return nvt_spi_write(ts, b, sizeof(b));
}

static int nvt_write_addr(struct nt36532e *ts, u32 addr, u8 val)
{
	u8 b[2] = { addr & 0x7f, val };
	int ret = nvt_set_page(ts, addr);
	return ret ? ret : nvt_spi_write(ts, b, sizeof(b));
}

static int nvt_read_addr(struct nt36532e *ts, u32 addr, u8 *val)
{
	u8 b[2] = { addr & 0x7f, 0 };
	int ret = nvt_set_page(ts, addr);
	if (!ret)
		ret = nvt_spi_read(ts, b, sizeof(b));
	if (!ret)
		*val = b[1];
	return ret;
}

static int nvt_detect(struct nt36532e *ts)
{
	u8 id[7] = { }, casc;
	int try, ret;

	for (try = 0; try < 5; try++) {
		ret = nvt_write_addr(ts, NVT_SWRST_SIF_ADDR, 0x69);
		if (ret) continue;
		msleep(5);
		ret = nvt_set_page(ts, NVT_CHIP_VER_TRIM_ADDR);
		if (ret) continue;
		id[0] = NVT_CHIP_VER_TRIM_ADDR & 0x7f;
		ret = nvt_spi_write(ts, id, sizeof(id));
		if (ret) continue;
		memset(id, 0, sizeof(id));
		id[0] = NVT_CHIP_VER_TRIM_ADDR & 0x7f;
		ret = nvt_spi_read(ts, id, sizeof(id));
		if (!ret && id[4] == 0x32 && id[5] == 0x65 && id[6] == 0x03)
			break;
		msleep(10);
	}
	if (try == 5)
		return dev_err_probe(&ts->spi->dev, -ENODEV, "NT36532E trim ID not found\n");
	ret = nvt_read_addr(ts, NVT_ENB_CASC_ADDR, &casc);
	if (ret) return ret;
	ts->cascade = !(casc & BIT(0));
	dev_info(&ts->spi->dev, "NT36532E %s detected\n", ts->cascade ? "cascade" : "single");
	return 0;
}

static int nvt_fw_needed_size(const struct firmware *fw, size_t *needed)
{
	int s;
	for (s = fw->size / NVT_SECTOR_SIZE; s > 0; s--) {
		size_t off = s * NVT_SECTOR_SIZE - 3;
		if (!memcmp(fw->data + off, "NVT", 3) || !memcmp(fw->data + off, "MOD", 3)) {
			*needed = s * NVT_SECTOR_SIZE;
			return 0;
		}
	}
	return -EINVAL;
}

static int nvt_parse_fw(const struct firmware *fw, struct nvt_partition **out,
			unsigned int *out_count, bool *second_header)
{
	struct nvt_partition *p;
	u32 end, pos;
	unsigned int info = 0, overlay = 0, count, i;
	bool second, found_header = false;

	if (fw->size < 0x40) return -EINVAL;
	end = nvt_le32(fw->data);
	if (end < 0x30 || end > fw->size) return -EINVAL;
	second = !!(fw->data[0x20] & BIT(1));
	*second_header = second;
	if (second) {
		for (pos = 0x30; pos < end / 2; pos += 0x10) info++;
		info++;
	} else {
		for (pos = 0x30; pos < end; pos += 0x10) info++;
	}
	if (fw->data[0x28] & BIT(4)) overlay = fw->data[0x28] & 0x0f;
	count = 2 + info + overlay;
	if (count > NVT_MAX_PARTITIONS) return -EINVAL;
	p = kcalloc(count, sizeof(*p), GFP_KERNEL);
	if (!p) return -ENOMEM;

	for (i = 0; i < 2; i++) {
		p[i].bin = nvt_le32(fw->data + i * 12);
		p[i].sram = nvt_le32(fw->data + i * 12 + 4);
		p[i].size = nvt_le32(fw->data + i * 12 + 8);
		p[i].crc = nvt_le32(fw->data + 0x18 + i * 4);
	}
	for (i = 0; i < info; i++) {
		unsigned int n = i + 2;
		u32 off = found_header && second ? end - 0x10 : 0x30 + i * 0x10;
		if ((u64)off + 0x10 > fw->size) goto bad;
		p[n].sram = nvt_le32(fw->data + off);
		p[n].size = nvt_le32(fw->data + off + 4);
		p[n].bin = nvt_le32(fw->data + off + 8);
		p[n].crc = nvt_le32(fw->data + off + 12);
		if (p[n].bin < end && p[n].size) found_header = true;
	}
	for (i = 0; i < overlay; i++) {
		unsigned int n = 2 + info + i;
		u32 off = p[1].bin + i * 0x10;
		if ((u64)off + 0x10 > fw->size) goto bad;
		p[n].sram = nvt_le32(fw->data + off);
		p[n].size = nvt_le32(fw->data + off + 4);
		p[n].bin = nvt_le32(fw->data + off + 8);
		p[n].crc = nvt_le32(fw->data + off + 12);
	}
	for (i = 0; i < count; i++)
		if (p[i].size && (u64)p[i].bin + p[i].size >= fw->size) goto bad;
	*out = p; *out_count = count;
	return 0;
bad:
	kfree(p);
	return -EINVAL;
}

static int nvt_write_sram(struct nt36532e *ts, const u8 *fw, u32 sram, u32 bytes, u32 bin)
{
	int ret;
	while (bytes) {
		u32 len = min_t(u32, bytes, NVT_XFER_LEN);
		ret = nvt_set_page(ts, sram);
		if (ret) return ret;
		ts->tx[0] = (sram & 0x7f) | 0x80;
		memcpy(ts->tx + 1, fw + bin, len);
		ret = spi_write(ts->spi, ts->tx, len + 1);
		if (ret) return ret;
		sram += len; bin += len; bytes -= len;
	}
	return 0;
}

static int nvt_crc_bank(struct nt36532e *ts, u32 des, u32 sram, u32 length,
			u32 size, u32 golden, u32 crc)
{
	u8 b[5];
	int ret = nvt_set_page(ts, des);
	if (ret) return ret;
	b[0]=des&0x7f; b[1]=sram; b[2]=sram>>8; b[3]=sram>>16;
	ret=nvt_spi_write(ts,b,4); if(ret)return ret;
	b[0]=length&0x7f; b[1]=size; b[2]=size>>8; b[3]=size>>16;
	ret=nvt_spi_write(ts,b,4); if(ret)return ret;
	b[0]=golden&0x7f; b[1]=crc; b[2]=crc>>8; b[3]=crc>>16; b[4]=crc>>24;
	return nvt_spi_write(ts,b,5);
}

static int nvt_wait_auto_copy(struct nt36532e *ts)
{
	u8 v; int i,ret;
	for(i=0;i<200;i++){
		ret=nvt_read_addr(ts,NVT_TX_AUTO_COPY_EN,&v); if(ret)return ret;
		if(!v)return 0;
		usleep_range(1000,2000);
	}
	return -ETIMEDOUT;
}

static int nvt_fw_crc_enable(struct nt36532e *ts)
{
	u8 b[2]; int ret=nvt_set_page(ts,NVT_EVENT_BUF_ADDR);
	if(ret)return ret;
	b[0]=NVT_EVENT_RESET_COMPLETE;b[1]=0;ret=nvt_spi_write(ts,b,2);if(ret)return ret;
	b[0]=NVT_EVENT_HOST_CMD;b[1]=0xae;return nvt_spi_write(ts,b,2);
}

static int nvt_wait_reset(struct nt36532e *ts)
{
	u8 b[6];int i,ret=nvt_set_page(ts,NVT_EVENT_BUF_ADDR|NVT_EVENT_RESET_COMPLETE);
	if(ret)return ret;
	for(i=0;i<=10;i++){
		memset(b,0,sizeof(b));b[0]=NVT_EVENT_RESET_COMPLETE;
		ret=nvt_spi_read(ts,b,sizeof(b));if(ret)return ret;
		if(b[1]>=NVT_RESET_STATE_INIT&&b[1]<=NVT_RESET_STATE_MAX)return 0;
		msleep(10);
	}
	return -ETIMEDOUT;
}

static int nvt_download_fw(struct nt36532e *ts)
{
	const struct firmware *fw;
	struct nvt_partition *p=NULL;
	unsigned int count=0,i;
	size_t need;
	bool second;
	int ret,attempt;

	ret=request_firmware(&fw,ts->fw_name,&ts->spi->dev);
	if(ret)return dev_err_probe(&ts->spi->dev,ret,"cannot load %s\n",ts->fw_name);
	ret=nvt_fw_needed_size(fw,&need);if(ret)goto out;
	if(need<NVT_SECTOR_SIZE||fw->data[need-NVT_SECTOR_SIZE]+fw->data[need-NVT_SECTOR_SIZE+1]!=0xff){ret=-ENOEXEC;goto out;}
	ret=nvt_parse_fw(fw,&p,&count,&second);if(ret)goto out;
	for(attempt=0;attempt<3;attempt++){
		ret=nvt_write_addr(ts,NVT_SWRST_SIF_ADDR,0x69);if(ret)continue;msleep(5);
		ret=nvt_crc_bank(ts,NVT_ILM_DES_ADDR,p[0].sram,NVT_ILM_LENGTH_ADDR,p[0].size,NVT_G_ILM_CSUM_ADDR,p[0].crc);if(ret)continue;
		ret=nvt_crc_bank(ts,NVT_DLM_DES_ADDR,p[1].sram,NVT_DLM_LENGTH_ADDR,p[1].size,NVT_G_DLM_CSUM_ADDR,p[1].crc);if(ret)continue;
		if(second){ret=nvt_write_addr(ts,NVT_TX_AUTO_COPY_EN,0x56);if(ret)continue;}
		for(i=0;i<count;i++){
			if(!p[i].size)continue;
			ret=nvt_write_sram(ts,fw->data,p[i].sram,p[i].size+1,p[i].bin);if(ret)break;
		}
		if(ret)continue;
		if(second&&(ret=nvt_wait_auto_copy(ts)))continue;
		ret=nvt_fw_crc_enable(ts);if(ret)continue;
		ret=nvt_write_addr(ts,NVT_BOOT_RDY_ADDR,1);if(ret)continue;msleep(5);
		ret=nvt_wait_reset(ts);if(!ret)break;
	}
	if(!ret)dev_info(&ts->spi->dev,"loaded %s (%u partitions)\n",ts->fw_name,count);
	else dev_err(&ts->spi->dev,"firmware download failed: %d\n",ret);
	kfree(p);
out:
	release_firmware(fw);return ret;
}

static bool nvt_point_checksum(const u8 *d)
{
	u8 sum=0,want;int i;
	for(i=0;i<NVT_POINT_CSUM_INDEX-1;i++)sum+=d[i+1];
	want=~sum+1;return want==d[NVT_POINT_CSUM_INDEX];
}

static bool nvt_pen_checksum(const u8 *d)
{
	u8 sum=0,want;int i;
	for(i=0;i<NVT_PEN_DATA_LEN-1;i++)sum+=d[NVT_PEN_DATA_OFFSET+i];
	want=~sum+1;return want==d[NVT_PEN_DATA_OFFSET+NVT_PEN_DATA_LEN-1];
}

static void nvt_pen_release(struct nt36532e *ts)
{
	if(!ts->pen)return;
	input_report_abs(ts->pen,ABS_PRESSURE,0);input_report_abs(ts->pen,ABS_DISTANCE,0);
	input_report_key(ts->pen,BTN_TOUCH,0);input_report_key(ts->pen,BTN_TOOL_PEN,0);
	input_report_key(ts->pen,BTN_STYLUS,0);input_report_key(ts->pen,BTN_STYLUS2,0);input_sync(ts->pen);
}

static void nvt_report_pen(struct nt36532e *ts,const u8 *d)
{
	u16 x,y,pressure,distance;u8 format;
	if(!ts->pen)return;
	format=d[66];if(format==0xff){nvt_pen_release(ts);return;}
	if(format!=0x01||!nvt_pen_checksum(d))return;
	x=((u16)d[67]<<8)|d[68];y=((u16)d[69]<<8)|d[70];pressure=((u16)d[71]<<8)|d[72];distance=((u16)d[75]<<8)|d[76];
	touchscreen_report_pos(ts->pen,&ts->prop,x,y,false);
	input_report_abs(ts->pen,ABS_PRESSURE,min_t(u16,pressure,ts->max_pressure));
	input_report_abs(ts->pen,ABS_TILT_X,(s8)d[73]);input_report_abs(ts->pen,ABS_TILT_Y,(s8)d[74]);input_report_abs(ts->pen,ABS_DISTANCE,distance);
	input_report_key(ts->pen,BTN_TOOL_PEN,1);input_report_key(ts->pen,BTN_TOUCH,pressure!=0);
	input_report_key(ts->pen,BTN_STYLUS,d[77]&BIT(0));input_report_key(ts->pen,BTN_STYLUS2,d[77]&BIT(1));input_sync(ts->pen);
}

static irqreturn_t nvt_irq(int irq,void *data)
{
	struct nt36532e *ts=data;u8 d[NVT_POINT_DATA_LEN+1]={0};unsigned long active=0;int i,ret;
	mutex_lock(&ts->lock);ret=nvt_set_page(ts,NVT_EVENT_BUF_ADDR);if(ret)goto out;
	d[0]=0;ret=nvt_spi_read(ts,d,sizeof(d));if(ret||!nvt_point_checksum(d))goto out;
	for(i=0;i<NVT_MAX_TOUCHES;i++){
		int pos=1+6*i,id=(d[pos]>>3)-1;u8 state=d[pos]&0x07;u32 x,y,pressure;
		if(id<0||id>=NVT_MAX_TOUCHES||(state!=0x01&&state!=0x02))continue;
		if(ts->high_res){x=((u32)d[pos+1]<<8)|d[pos+2];y=((u32)d[pos+3]<<8)|d[pos+4];pressure=d[pos+5]?:1;}
		else{x=((u32)d[pos+1]<<4)|(d[pos+3]>>4);y=((u32)d[pos+2]<<4)|(d[pos+3]&0x0f);pressure=d[pos+5];if(i<2)pressure+=(u32)d[i+63]<<8;pressure=clamp_val(pressure,1,1000);}
		if(x>ts->max_x||y>ts->max_y)continue;
		active|=BIT(id);input_mt_slot(ts->input,id);input_mt_report_slot_state(ts->input,MT_TOOL_FINGER,true);
		touchscreen_report_pos(ts->input,&ts->prop,x,y,true);input_report_abs(ts->input,ABS_MT_PRESSURE,pressure);
	}
	input_mt_sync_frame(ts->input);input_report_key(ts->input,BTN_TOUCH,!!active);input_sync(ts->input);
	if(ts->pen_support)nvt_report_pen(ts,d);
out:
	mutex_unlock(&ts->lock);return IRQ_HANDLED;
}

static int nvt_input_init(struct nt36532e *ts)
{
	struct device *dev=&ts->spi->dev;int ret;
	ts->input=devm_input_allocate_device(dev);if(!ts->input)return -ENOMEM;
	ts->input->name="Novatek NT36532E Touchscreen";ts->input->id.bustype=BUS_SPI;
	input_set_abs_params(ts->input,ABS_MT_POSITION_X,0,ts->max_x,0,0);input_set_abs_params(ts->input,ABS_MT_POSITION_Y,0,ts->max_y,0,0);input_set_abs_params(ts->input,ABS_MT_PRESSURE,0,1000,0,0);input_set_capability(ts->input,EV_KEY,BTN_TOUCH);__set_bit(INPUT_PROP_DIRECT,ts->input->propbit);
	touchscreen_parse_properties(ts->input,true,&ts->prop);ret=input_mt_init_slots(ts->input,NVT_MAX_TOUCHES,INPUT_MT_DIRECT);if(ret)return ret;ret=input_register_device(ts->input);if(ret||!ts->pen_support)return ret;
	ts->pen=devm_input_allocate_device(dev);if(!ts->pen)return -ENOMEM;
	ts->pen->name="Novatek NT36532E Pen";ts->pen->id.bustype=BUS_SPI;
	input_set_abs_params(ts->pen,ABS_X,0,ts->max_x,0,0);input_set_abs_params(ts->pen,ABS_Y,0,ts->max_y,0,0);input_set_abs_params(ts->pen,ABS_PRESSURE,0,ts->max_pressure,0,0);input_set_abs_params(ts->pen,ABS_TILT_X,-127,127,0,0);input_set_abs_params(ts->pen,ABS_TILT_Y,-127,127,0,0);input_set_abs_params(ts->pen,ABS_DISTANCE,0,65535,0,0);
	input_set_capability(ts->pen,EV_KEY,BTN_TOUCH);input_set_capability(ts->pen,EV_KEY,BTN_TOOL_PEN);input_set_capability(ts->pen,EV_KEY,BTN_STYLUS);input_set_capability(ts->pen,EV_KEY,BTN_STYLUS2);__set_bit(INPUT_PROP_DIRECT,ts->pen->propbit);
	return input_register_device(ts->pen);
}

static int nt36532e_probe(struct spi_device *spi)
{
	struct nt36532e *ts;int ret;
	ts=devm_kzalloc(&spi->dev,sizeof(*ts),GFP_KERNEL);if(!ts)return -ENOMEM;ts->spi=spi;mutex_init(&ts->lock);spi_set_drvdata(spi,ts);
	ts->xfer_size=NVT_XFER_LEN+2;ts->tx=devm_kmalloc(&spi->dev,ts->xfer_size,GFP_KERNEL);ts->rx=devm_kmalloc(&spi->dev,ts->xfer_size,GFP_KERNEL);if(!ts->tx||!ts->rx)return -ENOMEM;
	if(device_property_read_u32(&spi->dev,"touchscreen-size-x",&ts->max_x))ts->max_x=21200;if(device_property_read_u32(&spi->dev,"touchscreen-size-y",&ts->max_y))ts->max_y=30000;if(device_property_read_u32(&spi->dev,"touchscreen-max-pressure",&ts->max_pressure))ts->max_pressure=4095;
	ts->pen_support=device_property_read_bool(&spi->dev,"novatek,pen-support");ts->high_res=ts->max_x>4095||ts->max_y>4095;if(device_property_read_string(&spi->dev,"firmware-name",&ts->fw_name))ts->fw_name="novatek/DT-novatek-nt36532.bin";
	spi->mode=SPI_MODE_0;spi->bits_per_word=8;ret=spi_setup(spi);if(ret)return ret;
	mutex_lock(&ts->lock);ret=nvt_detect(ts);if(!ret)ret=nvt_download_fw(ts);mutex_unlock(&ts->lock);if(ret)return ret;
	ret=nvt_input_init(ts);if(ret)return ret;
	return devm_request_threaded_irq(&spi->dev,spi->irq,NULL,nvt_irq,IRQF_ONESHOT|IRQF_TRIGGER_FALLING,dev_name(&spi->dev),ts);
}

static int nt36532e_suspend(struct device *dev)
{
	struct spi_device *spi=to_spi_device(dev);struct nt36532e *ts=spi_get_drvdata(spi);u8 cmd[2]={NVT_EVENT_HOST_CMD,NVT_CMD_SLEEP};
	disable_irq(spi->irq);mutex_lock(&ts->lock);if(!nvt_set_page(ts,NVT_EVENT_BUF_ADDR))nvt_spi_write(ts,cmd,sizeof(cmd));nvt_pen_release(ts);mutex_unlock(&ts->lock);return 0;
}
static int nt36532e_resume(struct device *dev)
{
	struct spi_device *spi=to_spi_device(dev);struct nt36532e *ts=spi_get_drvdata(spi);int ret;
	mutex_lock(&ts->lock);ret=nvt_download_fw(ts);mutex_unlock(&ts->lock);if(!ret)enable_irq(spi->irq);return ret;
}
static DEFINE_SIMPLE_DEV_PM_OPS(nt36532e_pm,nt36532e_suspend,nt36532e_resume);
static const struct of_device_id nt36532e_of_match[]={{.compatible="novatek,nt36532e"},{}};
MODULE_DEVICE_TABLE(of,nt36532e_of_match);
static struct spi_driver nt36532e_driver={.driver={.name="nt36532e",.of_match_table=nt36532e_of_match,.pm=pm_sleep_ptr(&nt36532e_pm)},.probe=nt36532e_probe};
module_spi_driver(nt36532e_driver);
MODULE_DESCRIPTION("Novatek NT36532E no-flash SPI touchscreen and pen");
MODULE_LICENSE("GPL");
