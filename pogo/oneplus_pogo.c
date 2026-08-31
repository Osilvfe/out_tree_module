// SPDX-License-Identifier: GPL-2.0-only
/* OnePlus/Oplus pogo keyboard + touchpad protocol over UART/serdev. */
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/serdev.h>

#define POGO_SYNC 0x55
#define POGO_TAIL 0xaa
#define POGO_START 0xf1
#define POGO_REPEAT 0xf2
#define POGO_END 0xfe
#define POGO_KBD_ADDR 0xa1
#define POGO_PAD_ADDR 0xa2
#define POGO_CMD_KEY 0x01
#define POGO_CMD_MEDIA 0x02
#define POGO_CMD_TOUCHPAD 0x03
#define POGO_CMD_SYNC_UPLOAD 0x2f
#define POGO_CRC_INIT 0xc596
#define POGO_MAX_FRAME 275
#define POGO_MAX_TOUCHES 5

struct pogo_media_map { u16 usage, key; };
static const struct pogo_media_map media_map[] = {
	{0x0070,KEY_BRIGHTNESSDOWN},{0x006f,KEY_BRIGHTNESSUP},{0x00e2,KEY_MUTE},
	{0x00ea,KEY_VOLUMEDOWN},{0x00e9,KEY_VOLUMEUP},{0x0224,KEY_BACK},
	{0x00cd,KEY_PLAYPAUSE},{0x00b5,KEY_NEXTSONG},{0x00b6,KEY_PREVIOUSSONG},
	{0x0244,KEY_APPSELECT},
};

struct oneplus_pogo {
	struct serdev_device *serdev;
	struct input_dev *kbd,*touchpad;
	struct gpio_desc *wake,*tx_en;
	struct regulator *vcc;
	u32 baud,x_max,y_max;
	u16 crc_init;
	u8 old_keys[8],old_media[4];
	u8 frame[POGO_MAX_FRAME];
	size_t frame_len,expected;
	u8 sync_count;
};

static const u8 keycode[256] = {
	[4]=KEY_A,[5]=KEY_B,[6]=KEY_C,[7]=KEY_D,[8]=KEY_E,[9]=KEY_F,[10]=KEY_G,[11]=KEY_H,
	[12]=KEY_I,[13]=KEY_J,[14]=KEY_K,[15]=KEY_L,[16]=KEY_M,[17]=KEY_N,[18]=KEY_O,[19]=KEY_P,
	[20]=KEY_Q,[21]=KEY_R,[22]=KEY_S,[23]=KEY_T,[24]=KEY_U,[25]=KEY_V,[26]=KEY_W,[27]=KEY_X,
	[28]=KEY_Y,[29]=KEY_Z,[30]=KEY_1,[31]=KEY_2,[32]=KEY_3,[33]=KEY_4,[34]=KEY_5,[35]=KEY_6,
	[36]=KEY_7,[37]=KEY_8,[38]=KEY_9,[39]=KEY_0,[40]=KEY_ENTER,[41]=KEY_ESC,[42]=KEY_BACKSPACE,
	[43]=KEY_TAB,[44]=KEY_SPACE,[45]=KEY_MINUS,[46]=KEY_EQUAL,[47]=KEY_LEFTBRACE,[48]=KEY_RIGHTBRACE,
	[49]=KEY_BACKSLASH,[51]=KEY_SEMICOLON,[52]=KEY_APOSTROPHE,[53]=KEY_GRAVE,[54]=KEY_COMMA,[55]=KEY_DOT,
	[56]=KEY_SLASH,[57]=KEY_CAPSLOCK,[58]=KEY_F1,[59]=KEY_F2,[60]=KEY_F3,[61]=KEY_F4,[62]=KEY_F5,
	[63]=KEY_F6,[64]=KEY_F7,[65]=KEY_F8,[66]=KEY_F9,[67]=KEY_F10,[68]=KEY_F11,[69]=KEY_F12,
	[70]=KEY_SYSRQ,[71]=KEY_SCROLLLOCK,[72]=KEY_PAUSE,[73]=KEY_INSERT,[74]=KEY_HOME,[75]=KEY_PAGEUP,
	[76]=KEY_DELETE,[77]=KEY_END,[78]=KEY_PAGEDOWN,[79]=KEY_RIGHT,[80]=KEY_LEFT,[81]=KEY_DOWN,[82]=KEY_UP,
	[83]=KEY_NUMLOCK,[84]=KEY_KPSLASH,[85]=KEY_KPASTERISK,[86]=KEY_KPMINUS,[87]=KEY_KPPLUS,[88]=KEY_KPENTER,
	[89]=KEY_KP1,[90]=KEY_KP2,[91]=KEY_KP3,[92]=KEY_KP4,[93]=KEY_KP5,[94]=KEY_KP6,[95]=KEY_KP7,
	[96]=KEY_KP8,[97]=KEY_KP9,[98]=KEY_KP0,[99]=KEY_KPDOT,
	[224]=KEY_LEFTCTRL,[225]=KEY_LEFTSHIFT,[226]=KEY_LEFTALT,[227]=KEY_LEFTMETA,
	[228]=KEY_RIGHTCTRL,[229]=KEY_RIGHTSHIFT,[230]=KEY_RIGHTALT,[231]=KEY_RIGHTMETA,
};

static u16 pogo_crc(u16 crc,const u8 *p,size_t len)
{
	int i;
	while(len--){u8 d=*p++;for(i=0;i<8;i++){if((((crc&0x8000)>>8)^(d&0x80))!=0)crc=(crc<<1)^0x8005;else crc<<=1;d<<=1;}}
	return crc;
}
static bool key_present(const u8 *r,u8 usage){int i;for(i=2;i<8;i++)if(r[i]==usage)return true;return false;}

static void report_keys(struct oneplus_pogo *p,const u8 *buf,size_t len)
{
	u8 now[8];int i;
	if(len<9)return;memcpy(now,buf+1,8);
	for(i=0;i<8;i++)input_report_key(p->kbd,keycode[224+i],!!(now[0]&BIT(i)));
	for(i=2;i<8;i++){
		u8 old=p->old_keys[i],cur=now[i];
		if(old>3&&!key_present(now,old)&&keycode[old])input_report_key(p->kbd,keycode[old],0);
		if(cur>3&&!key_present(p->old_keys,cur)&&keycode[cur])input_report_key(p->kbd,keycode[cur],1);
	}
	memcpy(p->old_keys,now,8);input_sync(p->kbd);
}

static u16 media_key(u16 usage){int i;for(i=0;i<ARRAY_SIZE(media_map);i++)if(media_map[i].usage==usage)return media_map[i].key;return KEY_RESERVED;}
static bool media_present(const u8 *r,u16 usage){int i;for(i=0;i<2;i++)if(((u16)r[i*2]|(u16)r[i*2+1]<<8)==usage)return true;return false;}
static void report_media(struct oneplus_pogo *p,const u8 *buf,size_t len)
{
	u8 now[4];int i;if(len<5)return;memcpy(now,buf+1,4);
	for(i=0;i<2;i++){
		u16 old=p->old_media[i*2]|(u16)p->old_media[i*2+1]<<8,cur=now[i*2]|(u16)now[i*2+1]<<8,k;
		if(old&&!media_present(now,old)&&(k=media_key(old))!=KEY_RESERVED)input_report_key(p->kbd,k,0);
		if(cur&&!media_present(p->old_media,cur)&&(k=media_key(cur))!=KEY_RESERVED)input_report_key(p->kbd,k,1);
	}
	memcpy(p->old_media,now,4);input_sync(p->kbd);
}

static void report_touchpad(struct oneplus_pogo *p,const u8 *buf,size_t len)
{
	const u8 *d;u8 rlen,fingers,buttons;int i;
	if(len<3)return;rlen=buf[0];if(!rlen||(size_t)rlen+1>len)return;d=buf+1;fingers=buf[rlen];
	if(fingers>POGO_MAX_TOUCHES||(size_t)fingers*5>=rlen)return;buttons=d[rlen-1];
	for(i=0;i<fingers;i++){
		const u8 *f=d+i*5;u8 id=(f[0]>>4)&0xf;bool down=!!(f[0]&BIT(1));u16 x=f[1]|(u16)f[2]<<8,y=f[3]|(u16)f[4]<<8;
		if(id>=POGO_MAX_TOUCHES)continue;input_mt_slot(p->touchpad,id);input_mt_report_slot_state(p->touchpad,MT_TOOL_FINGER,down);
		if(down){input_report_abs(p->touchpad,ABS_MT_POSITION_X,min_t(u16,x,p->x_max));input_report_abs(p->touchpad,ABS_MT_POSITION_Y,min_t(u16,y,p->y_max));}
	}
	input_report_key(p->touchpad,BTN_LEFT,buttons&BIT(0));input_report_key(p->touchpad,BTN_RIGHT,buttons&BIT(1));input_mt_sync_frame(p->touchpad);input_sync(p->touchpad);
}

static void handle_frame(struct oneplus_pogo *p)
{
	u8 *f=p->frame,plen,cmd;u16 got,calc;const u8 *payload;
	if(p->frame_len<20||(f[8]!=POGO_START&&f[8]!=POGO_REPEAT))return;
	if(f[9]!=POGO_KBD_ADDR||f[10]!=POGO_PAD_ADDR)return;
	cmd=f[11];plen=f[12];if(p->frame_len!=plen+20)return;
	got=(u16)f[13+plen]<<8|f[14+plen];calc=pogo_crc(p->crc_init,f+8,plen+5);if(got!=calc)return;
	if(f[15+plen]!=POGO_END||f[16+plen]!=POGO_TAIL||f[17+plen]!=POGO_TAIL||f[18+plen]!=POGO_TAIL||f[19+plen]!=POGO_TAIL)return;
	payload=f+13;
	switch(cmd){case POGO_CMD_KEY:report_keys(p,payload,plen);break;case POGO_CMD_MEDIA:report_media(p,payload,plen);break;case POGO_CMD_TOUCHPAD:report_touchpad(p,payload,plen);break;case POGO_CMD_SYNC_UPLOAD:dev_dbg(&p->serdev->dev,"sync/heartbeat len=%u\n",plen);break;default:dev_dbg_ratelimited(&p->serdev->dev,"unhandled cmd 0x%02x\n",cmd);}
}

static void rx_byte(struct oneplus_pogo *p,u8 b)
{
	if(!p->frame_len){if(b==POGO_SYNC){if(++p->sync_count==8){memset(p->frame,POGO_SYNC,8);p->frame_len=8;p->sync_count=0;}}else p->sync_count=0;return;}
	if(p->frame_len>=sizeof(p->frame)){p->frame_len=p->expected=0;return;}
	p->frame[p->frame_len++]=b;if(p->frame_len==13)p->expected=p->frame[12]+20;
	if(p->expected&&p->frame_len==p->expected){handle_frame(p);p->frame_len=p->expected=0;}
}
static size_t pogo_receive(struct serdev_device *s,const u8 *buf,size_t count){struct oneplus_pogo *p=serdev_device_get_drvdata(s);size_t i;for(i=0;i<count;i++)rx_byte(p,buf[i]);return count;}
static const struct serdev_device_ops pogo_ops={.receive_buf=pogo_receive};

static int pogo_inputs(struct oneplus_pogo *p)
{
	struct device *dev=&p->serdev->dev;int i,ret;
	p->kbd=devm_input_allocate_device(dev);if(!p->kbd)return -ENOMEM;p->kbd->name="OnePlus Pogo Keyboard";p->kbd->id.bustype=BUS_RS232;
	for(i=0;i<ARRAY_SIZE(keycode);i++)if(keycode[i])input_set_capability(p->kbd,EV_KEY,keycode[i]);for(i=0;i<ARRAY_SIZE(media_map);i++)input_set_capability(p->kbd,EV_KEY,media_map[i].key);
	ret=input_register_device(p->kbd);if(ret)return ret;
	p->touchpad=devm_input_allocate_device(dev);if(!p->touchpad)return -ENOMEM;p->touchpad->name="OnePlus Pogo Touchpad";p->touchpad->id.bustype=BUS_RS232;__set_bit(INPUT_PROP_POINTER,p->touchpad->propbit);__set_bit(INPUT_PROP_BUTTONPAD,p->touchpad->propbit);
	input_set_capability(p->touchpad,EV_KEY,BTN_LEFT);input_set_capability(p->touchpad,EV_KEY,BTN_RIGHT);input_set_abs_params(p->touchpad,ABS_MT_POSITION_X,0,p->x_max,0,0);input_set_abs_params(p->touchpad,ABS_MT_POSITION_Y,0,p->y_max,0,0);
	ret=input_mt_init_slots(p->touchpad,POGO_MAX_TOUCHES,INPUT_MT_POINTER);return ret?ret:input_register_device(p->touchpad);
}

static int oneplus_pogo_probe(struct serdev_device *serdev)
{
	struct oneplus_pogo *p;u32 v;int ret;
	p=devm_kzalloc(&serdev->dev,sizeof(*p),GFP_KERNEL);if(!p)return -ENOMEM;p->serdev=serdev;serdev_device_set_drvdata(serdev,p);
	if(device_property_read_u32(&serdev->dev,"current-speed",&p->baud))return dev_err_probe(&serdev->dev,-EINVAL,"current-speed required until Caihong baud is confirmed\n");
	if(device_property_read_u32(&serdev->dev,"touchpad-size-x",&p->x_max))p->x_max=4096;if(device_property_read_u32(&serdev->dev,"touchpad-size-y",&p->y_max))p->y_max=4096;
	p->crc_init=POGO_CRC_INIT;if(!device_property_read_u32(&serdev->dev,"oneplus,crc-ibm-init",&v))p->crc_init=v;
	p->wake=devm_gpiod_get_optional(&serdev->dev,"wake",GPIOD_IN);if(IS_ERR(p->wake))return PTR_ERR(p->wake);p->tx_en=devm_gpiod_get_optional(&serdev->dev,"tx-enable",GPIOD_OUT_LOW);if(IS_ERR(p->tx_en))return PTR_ERR(p->tx_en);
	p->vcc=devm_regulator_get_optional(&serdev->dev,"vcc");if(IS_ERR(p->vcc)){if(PTR_ERR(p->vcc)==-ENODEV)p->vcc=NULL;else return dev_err_probe(&serdev->dev,PTR_ERR(p->vcc),"vcc\n");}
	if(p->vcc&&(ret=regulator_enable(p->vcc)))return ret;ret=pogo_inputs(p);if(ret)goto poweroff;
	serdev_device_set_client_ops(serdev,&pogo_ops);ret=devm_serdev_device_open(&serdev->dev,serdev);if(ret)goto poweroff;serdev_device_set_flow_control(serdev,false);serdev_device_set_baudrate(serdev,p->baud);dev_info(&serdev->dev,"pogo receiver ready at %u baud\n",p->baud);return 0;
poweroff:if(p->vcc)regulator_disable(p->vcc);return ret;
}
static void oneplus_pogo_remove(struct serdev_device *s){struct oneplus_pogo *p=serdev_device_get_drvdata(s);if(p->vcc)regulator_disable(p->vcc);}
static const struct of_device_id pogo_of_match[]={{.compatible="oneplus,caihong-pogo"},{.compatible="tinno,pogo_keyboard"},{}};
MODULE_DEVICE_TABLE(of,pogo_of_match);
static struct serdev_device_driver pogo_driver={.driver={.name="oneplus-pogo",.of_match_table=pogo_of_match},.probe=oneplus_pogo_probe,.remove=oneplus_pogo_remove};
module_serdev_device_driver(pogo_driver);
MODULE_DESCRIPTION("OnePlus/Oplus pogo keyboard and touchpad UART driver");
MODULE_LICENSE("GPL");
