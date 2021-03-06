#include <errno.h>
#include <dmacman.h>
#include <stdio.h>
#include <dev9.h>
#include <intrman.h>
#include <loadcore.h>
#include <modload.h>
#include <stdio.h>
#include <sysclib.h>
#include <thbase.h>
#include <thevent.h>
#include <thsemap.h>
#include <irx.h>

#include <smapregs.h>
#include <speedregs.h>

#include <ps2ip.h>

#include "main.h"

#include "xfer.h"

extern void *_gp;
extern struct SmapDriverData SmapDriverData;

static inline int CopyToFIFOWithDMA(volatile u8 *smap_regbase, void *buffer, int length){
	int NumBlocks;
	int result;

	if(((unsigned int)buffer&3)==0 && (NumBlocks=length>>7)>0){
		if(dev9DmaTransfer(1, buffer, NumBlocks<<16|0x20, DMAC_FROM_MEM)>=0){
			result=NumBlocks<<7;
		}
		else result=0;
	}
	else result=0;

	return result;
}

static inline int CopyFromFIFOWithDMA(volatile u8 *smap_regbase, void *buffer, int length){
	int result;
	USE_SPD_REGS;
	unsigned short int OldDMACtrl;

	//Attempt to steal the DMA channel from the DEV9 module.
	if((result=length/128)>0){
		OldDMACtrl=SPD_REG16(SPD_R_DMA_CTRL);

		while(dmac_ch_get_chcr(IOP_DMAC_8)&DMAC_CHCR_TR){}

		SPD_REG16(SPD_R_DMA_CTRL) = 7;	//SPEED revision 17 (ES2) and above only.

		SMAP_REG16(SMAP_R_RXFIFO_SIZE)=result;
		SMAP_REG8(SMAP_R_RXFIFO_CTRL)=SMAP_RXFIFO_DMAEN;

		dmac_request(IOP_DMAC_8, buffer, 0x20, result, DMAC_TO_MEM);
		dmac_transfer(IOP_DMAC_8);

		result*=128;

		/* Wait for DMA to complete. Do not use a semaphore as thread switching hurts throughput greatly.  */
		while(dmac_ch_get_chcr(IOP_DMAC_8)&DMAC_CHCR_TR){}
		while(SMAP_REG8(SMAP_R_RXFIFO_CTRL )&SMAP_RXFIFO_DMAEN){};

		SPD_REG16(SPD_R_DMA_CTRL)=OldDMACtrl;
	}
	else result=0;

	return result;
}

static inline void CopyFromFIFO(volatile u8 *smap_regbase, void *buffer, unsigned int length, unsigned short int RxBdPtr){
	int result;

	SMAP_REG16(SMAP_R_RXFIFO_RD_PTR)=RxBdPtr;

	if((result=CopyFromFIFOWithDMA(smap_regbase, buffer, length))>0){
		length-=result;
		(unsigned int)buffer+=result;
	}

	__asm__ __volatile__(
	".set noreorder\n\t"
	".set nomacro\n\t"
	".set noat\n\t"
	"lui      $v0, 0xB000\n\t"
	"srl      $at, %1, 5\n\t"
	"blez     $at, 3f\n\t"
	"andi     %1, %1, 0x1F\n\t"
	"4:\n\t"
	"lw      $t0, 4608($v0)\n\t"
	"lw      $t1, 4608($v0)\n\t"
	"lw      $t2, 4608($v0)\n\t"
	"lw      $t3, 4608($v0)\n\t"
	"lw      $t4, 4608($v0)\n\t"
	"lw      $t5, 4608($v0)\n\t"
	"lw      $t6, 4608($v0)\n\t"
	"lw      $t7, 4608($v0)\n\t"
	"addiu   $at, $at, -1\n\t"
	"sw      $t0,  0(%0)\n\t"
	"sw      $t1,  4(%0)\n\t"
	"sw      $t2,  8(%0)\n\t"
	"sw      $t3, 12(%0)\n\t"
	"sw      $t4, 16(%0)\n\t"
	"sw      $t5, 20(%0)\n\t"
	"sw      $t6, 24(%0)\n\t"
	"sw      $t7, 28(%0)\n\t"
	"bgtz    $at, 4b\n\t"
	"addiu   %0, %0, 32\n\t"
	"3:\n\t"
	"blez     %1, 1f\n\t"
	"nop\n\t"
	"2:\n\t"
	"lw      $t0, 4608($v0)\n\t"
	"addiu   %1, %1, -4\n\t"
	"sw      $t0, 0(%0)\n\t"
	"bgtz    %1, 2b\n\t"
	"addiu   %0, %0, 4\n\t"
	"1:\n\t"
	".set at\n\t"
	".set macro\n\t"
	".set reorder\n\t"
	:: "r"(buffer), "r"(length) : "at", "v0", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7"
	);
}

inline int HandleRxIntr(struct SmapDriverData *SmapDrivPrivData){
	USE_SMAP_RX_BD;
	int NumPacketsReceived;
	volatile smap_bd_t *PktBdPtr;
	volatile u8 *smap_regbase;
	struct pbuf* pbuf;
	unsigned short int ctrl_stat;

	smap_regbase=SmapDrivPrivData->smap_regbase;

	NumPacketsReceived=0;

	while(1){
		PktBdPtr=&rx_bd[SmapDrivPrivData->RxBDIndex&(SMAP_BD_MAX_ENTRY-1)];
		if(!((ctrl_stat=PktBdPtr->ctrl_stat)&SMAP_BD_RX_EMPTY)){
			if(ctrl_stat&(SMAP_BD_RX_INRANGE|SMAP_BD_RX_OUTRANGE|SMAP_BD_RX_FRMTOOLONG|SMAP_BD_RX_BADFCS|SMAP_BD_RX_ALIGNERR|SMAP_BD_RX_SHORTEVNT|SMAP_BD_RX_RUNTFRM|SMAP_BD_RX_OVERRUN) || PktBdPtr->length>MAX_FRAME_SIZE){
			}
			else{
				if((pbuf=pbuf_alloc(PBUF_RAW, PktBdPtr->length, PBUF_POOL))!=NULL){
					CopyFromFIFO(SmapDrivPrivData->smap_regbase, pbuf->payload, pbuf->len, PktBdPtr->pointer);

					//Inform ps2ip that we've received data.
					SMapLowLevelInput(pbuf);

					NumPacketsReceived++;
				}
			}

			SMAP_REG8(SMAP_R_RXFIFO_FRAME_DEC)=0;
			PktBdPtr->ctrl_stat=SMAP_BD_RX_EMPTY;
			SmapDrivPrivData->RxBDIndex++;
		}
		else break;
	}

	return NumPacketsReceived;
}

int SMAPSendPacket(const void *data, unsigned int length){
	int result;
	USE_SMAP_TX_BD;
	volatile u8 *smap_regbase;
	volatile u8 *emac3_regbase;
	volatile smap_bd_t *BD_ptr;
	u16 BD_data_ptr;
	unsigned int SizeRounded;

	if(SmapDriverData.SmapIsInitialized){
		SizeRounded=(length+3)&~3;
		smap_regbase=SmapDriverData.smap_regbase;
		emac3_regbase=SmapDriverData.emac3_regbase;

		while(SMAP_EMAC3_GET(SMAP_R_EMAC3_TxMODE0)&SMAP_E3_TX_GNP_0){};

		BD_data_ptr=SMAP_REG16(SMAP_R_TXFIFO_WR_PTR);
		BD_ptr=&tx_bd[SmapDriverData.TxBDIndex&0x3F];

		if((result=CopyToFIFOWithDMA(SmapDriverData.smap_regbase, (void*)data, length))>0){
			SizeRounded-=result;
			(unsigned int)data+=result;
		}

		__asm__ __volatile__(
			".set noreorder\n\t"
			".set nomacro\n\t"
			".set noat\n\t"
			"srl     $at, %1, 4\n\t"
			"lui     $v1, 0xB000\n\t"
			"beqz    $at, 3f\n\t"
			"andi    %1, %1, 0xF\n\t"
		"4:\n\t"
			"lwr     $t0,  0(%0)\n\t"
			"lwl     $t0,  3(%0)\n\t"
			"lwr     $t1,  4(%0)\n\t"
			"lwl     $t1,  7(%0)\n\t"
			"lwr     $t2,  8(%0)\n\t"
			"lwl     $t2, 11(%0)\n\t"
			"lwr     $t3, 12(%0)\n\t"
			"lwl     $t3, 15(%0)\n\t"
			"addiu   $at, $at, -1\n\t"
			"sw      $t0, 4352($v1)\n\t"
			"sw      $t1, 4352($v1)\n\t"
			"sw      $t2, 4352($v1)\n\t"
			"addiu   %0, %0, 16\n\t"
			"bgtz    $at, 4b\n\t"
			"sw      $t3, 4352($v1)\n\t"
		"3:\n\t"
			"beqz    %1, 1f\n\t"
			"nop\n\t"
		"2:\n\t"
			"lwr     $v0, 0(%0)\n\t"
			"lwl     $v0, 3(%0)\n\t"
			"addiu   %1, %1, -4\n\t"
			"sw      $v0, 4352($v1)\n\t"
			"bnez    %1, 2b\n\t"
			"addiu   %0, %0, 4\n\t"
		"1:\n\t"
			".set reorder\n\t"
			".set macro\n\t"
			".set at\n\t"
			:: "r"(data), "r"(SizeRounded) : "at", "v0", "v1", "t0", "t1", "t2", "t3"
		);

		BD_ptr->length=length;
		BD_ptr->pointer=BD_data_ptr;
		SMAP_REG8(SMAP_R_TXFIFO_FRAME_INC)=0;
		BD_ptr->ctrl_stat=SMAP_BD_TX_READY|SMAP_BD_TX_GENFCS|SMAP_BD_TX_GENPAD;	/* 0x8300 */
		SmapDriverData.TxBDIndex++;

		SMAP_EMAC3_SET(SMAP_R_EMAC3_TxMODE0, SMAP_E3_TX_GNP_0);

		result=1;
	}
	else result=-1;

	return result;
}

