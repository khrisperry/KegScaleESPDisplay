#include "platform.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
uint8_t VL53L7CX_WrMulti(VL53L7CX_Platform *p,uint16_t reg,uint8_t *v,uint32_t n){
  uint8_t out[VL53L7CX_I2C_CHUNK+2];
  for(uint32_t offset=0;offset<n;){
    uint32_t chunk=n-offset;if(chunk>VL53L7CX_I2C_CHUNK)chunk=VL53L7CX_I2C_CHUNK;
    uint16_t address=reg+offset;out[0]=address>>8;out[1]=address;
    memcpy(out+2,v+offset,chunk);
    if(!p->handle || i2c_master_transmit(p->handle,out,chunk+2,VL53L7CX_I2C_TIMEOUT)!=ESP_OK)return 1;
    offset+=chunk;
  }return 0;
}
uint8_t VL53L7CX_RdMulti(VL53L7CX_Platform *p,uint16_t reg,uint8_t *v,uint32_t n){
  for(uint32_t offset=0;offset<n;){
    uint32_t chunk=n-offset;if(chunk>VL53L7CX_I2C_CHUNK)chunk=VL53L7CX_I2C_CHUNK;
    uint16_t address=reg+offset;uint8_t a[2]={address>>8,address&255};
    if(!p->handle || i2c_master_transmit_receive(p->handle,a,2,v+offset,chunk,VL53L7CX_I2C_TIMEOUT)!=ESP_OK)return 1;
    offset+=chunk;
  }return 0;
}
uint8_t VL53L7CX_WrByte(VL53L7CX_Platform *p,uint16_t r,uint8_t v){return VL53L7CX_WrMulti(p,r,&v,1);}
uint8_t VL53L7CX_RdByte(VL53L7CX_Platform *p,uint16_t r,uint8_t *v){return VL53L7CX_RdMulti(p,r,v,1);}
uint8_t VL53L7CX_WaitMs(VL53L7CX_Platform *p,uint32_t ms){(void)p;vTaskDelay(pdMS_TO_TICKS(ms)+1);return 0;}
void VL53L7CX_SwapBuffer(uint8_t *b,uint32_t n){for(uint32_t i=0;i+3<n;i+=4){uint8_t t=b[i];b[i]=b[i+3];b[i+3]=t;t=b[i+1];b[i+1]=b[i+2];b[i+2]=t;}}
