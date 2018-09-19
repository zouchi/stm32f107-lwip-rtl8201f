#include "lwip/opt.h"
#include "lwip/def.h"
#include "lwip/mem.h"
#include "lwip/pbuf.h"
#include <lwip/stats.h>
#include <lwip/snmp.h>
#include "netif/etharp.h"
#include "netif/ppp_oe.h"
#include "stm32_eth.h"
#include <string.h>

//网卡的名字
#define IFNAME0 'e'
#define IFNAME1 'n'

#define  ETH_DMARxDesc_FrameLengthShift           16         //DMA接收描述符，RDES0软件寄存器中描述帧长度的位的偏移值
#define  ETH_ERROR                                ((u32)0)   //出错代码
#define  ETH_SUCCESS                              ((u32)1)   //无错代码

#define ETH_RXBUFNB        5  //接收缓冲器数量
#define ETH_TXBUFNB        5  //发送缓冲器数量

extern u8_t MACaddr[6];                           //MAC地址，具有唯一性
extern ETH_DMADESCTypeDef  *DMATxDescToSet;  //当前DMA发送描述符指针，在以太网库文件中定义的
extern ETH_DMADESCTypeDef  *DMARxDescToGet;  //当前DMA接收描述符指针，在以太网库文件中定义的

ETH_DMADESCTypeDef  DMARxDscrTab[ETH_RXBUFNB], DMATxDscrTab[ETH_TXBUFNB];                    //发送和接收DMA描述符数组
uint8_t Rx_Buff[ETH_RXBUFNB][ETH_MAX_PACKET_SIZE], Tx_Buff[ETH_TXBUFNB][ETH_MAX_PACKET_SIZE];//发送和接收缓冲区

//数据帧结构体，和我们使用的网卡相关
typedef struct{
u32_t length;                     //帧长度
u32_t buffer;                     //缓冲区
ETH_DMADESCTypeDef *descriptor;   //指向DMA描述符的指针
}FrameTypeDef;


//前置的函数声明
FrameTypeDef ETH_RxPkt_ChainMode(void);       //网卡接收数据
u32_t ETH_GetCurrentTxBuffer(void);           //获取当前DMA发送描述符下数据缓冲区指针
u32_t ETH_TxPkt_ChainMode(u16 FrameLength);   //网卡发送数据
err_t ethernetif_input(struct netif *netif);  //上层接口函数

//初始化函数
static void low_level_init(struct netif *netif)      
{
  uint8_t i;
	
  netif->hwaddr_len = ETHARP_HWADDR_LEN;  //设置MAC地址长度

  netif->hwaddr[0] = MACaddr[0];  //设置MAC地址，6位，地址唯一，不能重复
  netif->hwaddr[1] = MACaddr[1];
  netif->hwaddr[2] = MACaddr[2];
  netif->hwaddr[3] = MACaddr[3];
  netif->hwaddr[4] = MACaddr[4];
  netif->hwaddr[5] = MACaddr[5];

  netif->mtu = 1500;   //最大传输单元
  
  //设置网卡功能
  //NETIF_FLAG_BROADCAST允许广播
  //NETIF_FLAG_ETHARP开启ARP功能
  //NETIF_FLAG_LINK_UP设置后接口产生一个活跃的链接，要开启硬件校验
  netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;
	
  //接下来我们要初始化发送和接收DMA描述符链表
  //107VCT6采用链式结构
  //我们要先创建DMA描述符数组
  //DMA描述符内包含了一个指向接收和发送缓冲区的指针，我们还要创建接收和发送缓冲区，两个数组
  ETH_DMATxDescChainInit(DMATxDscrTab, &Tx_Buff[0][0], ETH_TXBUFNB);//初始化发送DMA描述符链表
  ETH_DMARxDescChainInit(DMARxDscrTab, &Rx_Buff[0][0], ETH_RXBUFNB);//初始化接收DMA描述符链表
	
	//开启DMA描述符接收中断
	for(i=0; i<ETH_RXBUFNB; i++)
	{
		ETH_DMARxDescReceiveITConfig(&DMARxDscrTab[i], ENABLE);
	}
	
#if  !CHECKSUM_GEN_ICMP    //判断是否开启硬件校验,关闭软件校验
    //开启发送帧校验 
	  for(i=0; i<ETH_TXBUFNB; i++)
    {
      ETH_DMATxDescChecksumInsertionConfig(&DMATxDscrTab[i], ETH_DMATxDesc_ChecksumTCPUDPICMPFull);
    }
#endif
		
	 ETH_Start();//开启MAC和DMA
}

//发送数据函数
//看一看简单的框架和DMA描述符的结构
//整理思路如下
//要发送的数据存放在最为参数传进来的pubf下
//DMA发送描述符内有指向缓冲器的指针，而且我们也设置了缓冲区
//我们首先要得到描述符的DMA缓冲区指针，所以我们要实现一个ETH_GetCurrentTxBuffer函数
//接下来我们将pbuf的数据拷贝到缓冲区
//根据使用的网卡，写一个网卡发送数据的函数ETH_TxPkt_ChainMode
//这几个函数ST官方都给了基于DP83848的例程
static err_t low_level_output(struct netif *netif, struct pbuf *p)
{
  struct pbuf *q; //顶一个pbuf缓冲区，暂存拷贝中间数据
  int l = 0;      //长度
  u8 *buffer =  (u8 *)ETH_GetCurrentTxBuffer();//获取当前DMA内缓冲区指针，将要发送的数据，拷贝到缓冲区
  
  for(q = p; q != NULL; q = q->next)   //利用for循环，拷贝数据
  {
    memcpy((u8_t*)&buffer[l], q->payload, q->len);
	l = l + q->len;
  }

  ETH_TxPkt_ChainMode(l);//发送数据

  return ERR_OK;
}

//接收数据函数
//看一看简单的框架和DMA描述符的结构
//整理思路如下
//当网卡接收到数据，会存放在接收缓冲区，接收DMA描述符下有指向其的指针
//我们还要实现一个网卡接收数据的函数ETH_TxPkt_ChainMode，同发送一样ST提供了例程
//得到缓冲区的数据后，我们要将其拷贝到pbuf结构中，供LWip使用
//所以我们最后将数据拷贝到pbuf后，将它作为函数返回值，返回
static struct pbuf * low_level_input(struct netif *netif)
{
  struct pbuf *p, *q; //p要返回的数据，q拷贝数据时用于暂存数据
  u16_t len;          //保存接收到数据帧的长度
  int l =0;           //长度，for时暂存中间值
  FrameTypeDef frame; //接受侦
  u8 *buffer;         //接收到数据的地址
  
  p = NULL; //p向指向空，待用
  frame = ETH_RxPkt_ChainMode();//接收数据帧

  len = frame.length;//将数据帧长度存放在len内待用
  
  buffer = (u8 *)frame.buffer; //得到数据区地址

  p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);//内存池分配空间

  if (p != NULL)//分配成功
  {
    for (q = p; q != NULL; q = q->next)//利用for循环拷贝数据
    {
	  memcpy((u8_t*)q->payload, (u8_t*)&buffer[l], q->len);
      l = l + q->len;
    }    
  }

  frame.descriptor->Status = ETH_DMARxDesc_OWN; //设置DMA占用描述符
 
  if ((ETH->DMASR & ETH_DMASR_RBUS) != (u32)RESET)  //通过判断ETH->DMASR寄存器位7，判断接收缓冲区可不可用
  {
    //接收缓冲区不可用，if成立
	ETH->DMASR = ETH_DMASR_RBUS; //清除接收缓冲区不可用标志
    ETH->DMARPDR = 0;//通过写ETH->DMARPDR寄存器，恢复DMA接收
  }

  return p;//返回数据
}

//LWip调用的接收数据函数
//主要就是调用了low_level_input函数
err_t ethernetif_input(struct netif *netif)
{
  err_t err;
  struct pbuf *p;

  p = low_level_input(netif);//调用low_level_input函数接收数据

  if (p == NULL) return ERR_MEM;//无数据可读，返回错误代码

  err = netif->input(p, netif);//调用LWip源码处理数据
  if (err != ERR_OK) //如果处理失败
  {
    LWIP_DEBUGF(NETIF_DEBUG, ("ethernetif_input: IP input error\n"));//调试信息
    pbuf_free(p);//释放掉pbuf空间
    p = NULL;
  }

  return err;
}


err_t ethernetif_init(struct netif *netif)
{
  LWIP_ASSERT("netif != NULL", (netif != NULL));

#if LWIP_NETIF_HOSTNAME
  netif->hostname = "lwip";//命名
#endif 

  //初始化netif相关字段
	netif->name[0] = IFNAME0;
  netif->name[1] = IFNAME1;
  netif->output = etharp_output;
  netif->linkoutput = low_level_output;

  low_level_init(netif);

  return ERR_OK;
}

//网卡接收数据函数
FrameTypeDef ETH_RxPkt_ChainMode(void)
{ 
  u32 framelength = 0;             //变量待用
  FrameTypeDef frame = {0,0};      //帧结构待用

  if((DMARxDescToGet->Status & ETH_DMARxDesc_OWN) != (u32)RESET)//如果DMA占用描述符成立
  {	
	frame.length = ETH_ERROR;   //存放错误代码

    if ((ETH->DMASR & ETH_DMASR_RBUS) != (u32)RESET)  //如果发送缓存不可用，if成立
    {
		ETH->DMASR = ETH_DMASR_RBUS; //清除接收缓冲区不可用标志
		ETH->DMARPDR = 0;//通过写ETH->DMARPDR寄存器，恢复DMA接收
    }
	
    return frame; //返回帧结构
  }
  //如果上步if不成立，标志描述符由CPU占用
  //又要进行3个判断
  //ETH_DMARxDesc_ES判断接收中是否出错，成立表示没有错误发生
  //ETH_DMARxDesc_LS判断是否到了最后一个缓冲区
  //ETH_DMARxDesc_FS判断是否包含了帧的第一个缓冲区
  if(((DMARxDescToGet->Status & ETH_DMARxDesc_ES) == (u32)RESET) && 
     ((DMARxDescToGet->Status & ETH_DMARxDesc_LS) != (u32)RESET) &&  
     ((DMARxDescToGet->Status & ETH_DMARxDesc_FS) != (u32)RESET))  
  {      
     //都成立的话，得到帧长度值，
	 //DMA接收描述符RDES0软件寄存器位16-位29存放帧长度值
	 //右移16位，然后还要减去4个自己的CRC校验
    framelength = ((DMARxDescToGet->Status & ETH_DMARxDesc_FL) >> ETH_DMARxDesc_FrameLengthShift) - 4;
	
	frame.buffer = DMARxDescToGet->Buffer1Addr;	//得到接收描述符下Buffer1Addr地址，它指向了数据缓冲区
  }
  else//如果上步if不成立
  {
    framelength = ETH_ERROR;//记录错误代码
  }

  frame.length = framelength; //将帧长度值，记录在frame结构体中的length成员


  frame.descriptor = DMARxDescToGet;//frame结构体中的descriptor成员指向当前的DMA接收描述符
  
  DMARxDescToGet = (ETH_DMADESCTypeDef*) (DMARxDescToGet->Buffer2NextDescAddr);//将当前接收DMA描述符指针，指向下一个接收DMA链表中的DMA描述符  

  return (frame);  //返回帧结构
}

//网卡发送数据函数
u32_t ETH_TxPkt_ChainMode(u16 FrameLength)
{   
  if((DMATxDescToSet->Status & ETH_DMATxDesc_OWN) != (u32)RESET)//如果DMA占用描述符成立
  {  
    return ETH_ERROR;//返回错误代码
  }
        
  //如果if不成立，表示CPU占用描述符
  DMATxDescToSet->ControlBufferSize = (FrameLength & ETH_DMATxDesc_TBS1);//设置发送帧长度

  DMATxDescToSet->Status |= ETH_DMATxDesc_LS | ETH_DMATxDesc_FS;//ETH_DMATxDesc_LS和ETH_DMATxDesc_FS置1，表示帧中存放了，第一个和最后一个分块

  DMATxDescToSet->Status |= ETH_DMATxDesc_OWN;//把描述符给DMA使用

  if ((ETH->DMASR & ETH_DMASR_TBUS) != (u32)RESET)//如果发送缓存不可用，if成立
  {
    ETH->DMASR = ETH_DMASR_TBUS;//清除发送缓存不可用标志
    ETH->DMATPDR = 0;//写ETH->DMATPDR寄存器，以求回复发送流程
  }

  DMATxDescToSet = (ETH_DMADESCTypeDef*) (DMATxDescToSet->Buffer2NextDescAddr);//将当前发送DMA描述符指针，指向下一个发送DMA链表中的DMA描述符     


  return ETH_SUCCESS;   //返回成功代码
}

//获取发送DMA描述符下的缓冲区
u32_t ETH_GetCurrentTxBuffer(void)
{ 
  return (DMATxDescToSet->Buffer1Addr);   //得到DMA描述符内Buffer1Addr地址。
}

