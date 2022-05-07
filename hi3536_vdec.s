<?xml version="1.0"?>
<hi3536_vdec>
  <BgColor type="int" discrete="{0x0,0xFF,0x8000}" alias="black,blue,green" save="true"/>
  <no_stream>
   <mode type="int" discrete="{0,1}" alias="none,image" cond="rerun" save="true"/>
   <file const="true"/>
  </no_stream>
  <inst>
    <vo>
      <IntfSync type="int" discrete="{6,7,8,9,10,11,12,30,31,32}" alias="1080p30,720p50,720p60,1080i50,1080i60,1080p50,1080p60,4kp30,4kp50,4kp60" cond="rerun" save="true"/>
      <IntfType type="int" discrete="{0x10,0x20}" alias="bt1120,hdmi" const="true"/>
      <mode type="int" discrete="{0,1,2}" alias="1x1,2x2,3x3" save="true"/>
      <channel type="int" discrete="{0,1,2,3,4,5,6,7,8}" save="true"/>
    </vo>
  </inst>
</hi3536_vdec>
