#include "digicam/storage_manager.h"
#include <img_converters.h>
namespace dc {
bool StorageManager::begin(){
  // True selects 1-bit mode. GPIO13 remains available for the button.
  ready_=SD_MMC.begin("/sdcard",true);
  if(ready_ && SD_MMC.cardType()==CARD_NONE) ready_=false;
  if(ready_ && !ensureDcim()) ready_=false;
  return ready_;
}
bool StorageManager::ensureDcim(){if(!SD_MMC.exists("/DCIM"))return SD_MMC.mkdir("/DCIM");return true;}
String StorageManager::nextPath(){
  char p[32];
  for(uint32_t tries=0;tries<99999;tries++){
    snprintf(p,sizeof(p),"/DCIM/IMG_%05lu.jpg",(unsigned long)nextNumber_++);
    if(nextNumber_>99999)nextNumber_=1;
    if(!SD_MMC.exists(p))return String(p);
  }
  return String();
}
size_t StorageManager::writeCallback(void* arg,size_t index,const void* data,size_t len){
  File* f=static_cast<File*>(arg);(void)index;if(!f||!*f)return 0;return f->write((const uint8_t*)data,len);
}
bool StorageManager::saveJpegFromYuv(uint8_t* yuv,size_t len,uint16_t w,uint16_t h,String& savedPath){
  savedPath="";
  if(!ready_||!yuv||w<2||h<2||(w&1U)||len<(size_t)w*h*2U||!ensureDcim())return false;
  String path=nextPath();if(path.length()==0)return false;String tmp=path+".tmp";if(SD_MMC.exists(tmp))SD_MMC.remove(tmp);
  File out=SD_MMC.open(tmp,FILE_WRITE);if(!out)return false;
  // The framework declares the input as mutable even though this encoder path reads it.
  // Pass the writable transformed frame directly; never pass the camera driver's raw FB here.
  bool converted=fmt2jpg_cb(yuv,len,w,h,PIXFORMAT_YUV422,92,writeCallback,&out);
  out.flush();size_t sz=out.size();out.close();
  bool markers=false;
  if(converted&&sz>500){File chk=SD_MMC.open(tmp,FILE_READ);if(chk&&chk.size()>=4){uint8_t head[2]={0},tail[2]={0};chk.read(head,2);chk.seek(chk.size()-2);chk.read(tail,2);markers=head[0]==0xFF&&head[1]==0xD8&&tail[0]==0xFF&&tail[1]==0xD9;}chk.close();}
  bool ok=converted&&markers&&SD_MMC.rename(tmp,path);
  if(!ok){SD_MMC.remove(tmp);return false;}
  savedPath=path;return true;
}
uint32_t StorageManager::freeBytes() const {if(!ready_)return 0;uint64_t total=SD_MMC.totalBytes(), used=SD_MMC.usedBytes();return used>=total?0:(uint32_t)(total-used);}
}
