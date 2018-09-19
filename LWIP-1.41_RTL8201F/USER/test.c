#include "stm32f10x.h"
#include "netif/etharp.h"
#include "lwip/dhcp.h"
#include "lwip/mem.h"
#include "lwip/memp.h"
#include "lwip/init.h"
#include "lwip/timers.h"
#include "lwip/tcp_impl.h"
#include "lwip/ip_frag.h"
#include "lwip/tcpip.h"
#include "stm32_eth.h"
#include <stdio.h>

void ETH_Reinit(void);

err_t ethernetif_init(struct netif *netif); //其他文件内函数声明
err_t ethernetif_input(struct netif *netif);//其他文件内函数声明

u8_t MACaddr[6]={0,0,0,0,0,1};              //MAC地址，具有唯一性

struct netif netif;             //定义一个全局的网络接口，注册网卡函数要用到  
__IO uint32_t TCPTimer = 0;     //TCP查询计时器 
__IO uint32_t ARPTimer = 0;     //ARP查询计时器
extern uint32_t LWipTime;       //LWip周期时钟
volatile uint8_t  IGMP_UP_Time; //断线重连时间表
uint8_t EthInitStatus;          //初始化状态

#if LWIP_DHCP                   //如果开启DHCP
u32 DHCPfineTimer=0;	        //DHCP精细处理计时器
u32 DHCPcoarseTimer=0;	        //DHCP粗糙处理计时器
#endif

void LWIP_Init(void)
{
  struct ip_addr ipaddr;     //定义IP地址结构体
  struct ip_addr netmask;    //定义子网掩码结构体
  struct ip_addr gw;         //定义网关结构体

  mem_init();  //初始化动态内存堆

  memp_init(); //初始化内存池

  lwip_init(); //初始化LWIP内核
	
#if LWIP_DHCP     //如果开启DHCP，自动获取IP
  ipaddr.addr = 0;
  netmask.addr = 0;
  gw.addr = 0;
#else            //不开启DHCP，使用静态IP
  IP4_ADDR(&ipaddr, 192, 168, 3, 30);      //设置IP地址
  IP4_ADDR(&netmask, 255, 255, 255, 0);   //设置子网掩码
  IP4_ADDR(&gw, 192, 168, 3, 1);          //设置网关
#endif

  ETH_MACAddressConfig(ETH_MAC_Address0, MACaddr);  //配置MAC地址

  //注册网卡函数   ethernetif_init函数，需要自己实现，用于配置netif相关字段，以及初始化底层硬件。
  netif_add(&netif, &ipaddr, &netmask, &gw, NULL, &ethernetif_init, &ethernet_input);

  //将注册网卡函数注册的网卡设置为默认网卡
  netif_set_default(&netif);


#if LWIP_DHCP           //如果开启了DHCP复位
  dhcp_start(&netif);   //开启DHCP
#endif

  //打开网卡
  netif_set_up(&netif);
}

//接收数据函数
void LwIP_Pkt_Handle(void)
{
  //从网络缓冲区中读取接收到的数据包并将其发送给LWIP处理 
  ethernetif_input(&netif);
}

/*******************************
函数功能：断线重连
********************************/
void Eth_Link_ITHandler(void)
{
/* Check whether the link interrupt has occurred or not */

   // if(((ETH_ReadPHYRegister(PHY_ADDRESS, PHY_BSR)) & PHY_BSR) != 0){/*检测插拔中断*/
    if((ETH_ReadPHYRegister(PHY_ADDRESS, PHY_BSR) & PHY_Linked_Status) == 0x00){
        uint16_t status = ETH_ReadPHYRegister(PHY_ADDRESS, PHY_BSR);
        if(status & (PHY_AutoNego_Complete | PHY_Linked_Status)){/*检测到网线连接*/
            if(EthInitStatus != 1){/*if(EthInitStatus != ETH_SUCCESS)之前未成功初始化过*/
                /*Reinit PHY*/
                ETH_Reinit();
            }
            else{/*之前已经成功初始化*/
                /*set link up for re link callbalk function*/
                netif_set_link_up(&netif);
            }
        }
        else{/*网线断开*/\
            /*设置链接以重新链接callbalk功能*/
            netif_set_link_down(&netif);
        }
        IGMP_UP_Time = 0;  //掉线标志位
    } 
}

//LWIP周期性任务
void lwip_periodic_handle()
{
#if LWIP_TCP
	//每250ms调用一次tcp_tmr()函数
  if (LWipTime - TCPTimer >= 250)
  {
    TCPTimer =  LWipTime;
    tcp_tmr();
  }
   #if LWIP_IGMP 
        if(IGMP_UP_Time>=2){
            IGMP_UP_Time=3;
            igmp_tmr();           
        } 
    #endif    
#endif
  //ARP每5s周期性调用一次
  if ((LWipTime - ARPTimer) >= ARP_TMR_INTERVAL)
  {
    ARPTimer =  LWipTime;
    etharp_tmr();
    IGMP_UP_Time++;
    Eth_Link_ITHandler();   //断线重连
  }

#if LWIP_DHCP //如果使用DHCP的话
  //每500ms调用一次dhcp_fine_tmr()
  if (LWipTime - DHCPfineTimer >= DHCP_FINE_TIMER_MSECS)
  {
    DHCPfineTimer =  LWipTime;
    dhcp_fine_tmr();
  }

  //每60s执行一次DHCP粗糙处理
  if (LWipTime - DHCPcoarseTimer >= DHCP_COARSE_TIMER_MSECS)
  {
    DHCPcoarseTimer =  LWipTime;
    dhcp_coarse_tmr();
  }  
#endif
}






