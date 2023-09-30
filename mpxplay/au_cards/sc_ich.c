//**************************************************************************
//*                     This file is part of the                           *
//*                      Mpxplay - audio player.                           *
//*                  The source code of Mpxplay is                         *
//*        (C) copyright 1998-2008 by PDSoft (Attila Padar)                *
//*                http://mpxplay.sourceforge.net                          *
//*                  email: mpxplay@freemail.hu                            *
//**************************************************************************
//*  This program is distributed in the hope that it will be useful,       *
//*  but WITHOUT ANY WARRANTY; without even the implied warranty of        *
//*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.                  *
//*  Please contact with the author (with me) if you want to use           *
//*  or modify this source.                                                *
//**************************************************************************
//function: Intel ICH audiocards low level routines
//based on: ALSA (http://www.alsa-project.org) and ICH-DOS wav player from Jeff Leyda

#define MPXPLAY_USE_DEBUGF 1
#define ICH_DEBUG_OUTPUT stdout

#include "mpxplay.h"
#include <time.h>

#include "sis7012_debug.h"

/**
 * SIS7012 WIP Notes:
 *
 * - The channel mask bits (2, 4, 6 channels) in the global control block
 *   (register 0x2c) are different -- which is important for masking out the
 *   bits, but not important for setting bits, as SBEMU only uses 2-channel
 *   audio, and 2-channel audio is set by clearing the 4-channel and 6-channel
 *   bits -- that's ICH_PCM_246_MASK in both MPXPlay and Linux, Linux has the
 *   mask as ICH_SIS_PCM_246_MASK
 *
 * - SIS7012 supports a "tertiary" codec with two additional flags (tertiary
 *   resume irq, tertiary codec ready)
 *
 * - Linux assigns the device_type of DEVICE_SIS based on the PCI ID (MPXPlay
 *   has basically the same enum, the PCI ID is just commented out)
 *
 * - In snd_intel8x0_setup_pcm_out(), the channel mask setting is different
 *   (see above)
 *
 * - In snd_intel8x0_pcm_open(), both buffer_bytes_max and period_bytes_max are
 *   set to 64 KiB -- seems like we create smaller buffers anyway, so SBEMU is
 *   probably not affected by this
 *
 * - In snd_intel8x0_ich_chip_init(), the LSB of register 0x4c is set, and the
 *   comment above it says "unmute the output on SIS7012", which is probably
 *   kind of important..
 *
 * - Then, the ich_codec_bits[] has the tertiary for ICH4, but there's
 *   sis_codec_bits[] which has its own tertiary codec (ICH_SIS_TCR) bit
 *
 * - In snd_intel8x0_init(), bdbars is 3 for DEVICE_INTEL and also 3 for
 *   DEVICE_SIS, so this is probably fine
 *
 * - SIS7012 swaps the PICB (position in current buffer, offset 0x08) and SR
 *   (status, offset 0x06) registers, this is put into "roff_sr" and
 *   "roff_picb" of the ichdev, and then used throughout; would need
 *   checking/updating to fix all the occurences and swap around
 *
 * - ichdev->pos_shift is 1 (divided by two) for normal ICH, and 0 (no division
 *   by two) for SIS7012, the comment above says "SIS7012 handles the pcm data
 *   in bytes, others are in samples" -- this also needs to be handled, and is
 *   mostly relevant in setting up DMA and when retrieving the PCM pointer (for
 *   writing?), there's also some reference to it in
 *   intel8x0_measure_ac97_clock(), but this seems to be only relevant for
 *   chips with a clock quirk
 *
 * - Then, when setting up max_codecs, codec_bit and codec_ready_bits, it also
 *   takes care of the tertiary codec, but it might work just fine with two
 *   codecs initialized (like normal ICH)
 *
 * References:
 *   Linux: https://github.com/torvalds/linux/blob/master/sound/pci/intel8x0.c
 *   FreeBSD: https://cgit.freebsd.org/src/tree/sys/dev/sound/pci/ich.c
 *   OSSv4: http://www.4front-tech.com/developer/sources/stable/gpl/oss-v4.2-build2020-src-gpl.tar.bz2
 *   JUDAS: https://github.com/volkertb/JUDAS/blob/master/JUDAS.C
 *   QEMU AC97: https://github.com/qemu/qemu/blob/master/hw/audio/ac97.c
 *   86Box: https://github.com/86Box/86Box/blob/master/src/sound/snd_ac97_codec.c
 *
 * Further:
 *   https://wiki.osdev.org/PCI
 *   https://wiki.osdev.org/AC97
 *
 * TODO Serial Debugging:
 *   https://github.com/MindlapseDemos/wip-dosdemo/blob/master/src/dos/logger.c#L107
 *   https://wiki.osdev.org/Serial_Ports
 */

#ifdef AU_CARDS_LINK_ICH

#include <string.h>
#include "dmairq.h"
#include "pcibios.h"
#include "ac97_def.h"

#define ICH_PO_CR_REG     0x1b  // PCM out Control Register ("Transfer Control" register)
#define ICH_PO_CR_START   0x01  // start codec
#define ICH_PO_CR_RESET   0x02  // reset codec
#define ICH_PO_CR_LVBIE   0x04  // last valid buffer interrupt enable
#define ICH_PO_CR_FEIE    0x08    /* fifo error interrupt enable */
#define ICH_PO_CR_IOCE    0x10  // IOC enable

// https://wiki.osdev.org/AC97#0x06_0x16_0x26_Transfer_Status
#define ICH_PO_SR_REG     ((card->device_type==DEVICE_SIS)?0x18:0x16)  // PCM out Status register ("Transfer Status")

#define ICH_PO_SR_DCH     0x01  // DMA controller halted (RO)
#define ICH_PO_SR_LVBCI   0x04  // last valid buffer completion interrupt (R/WC)
#define ICH_PO_SR_BCIS    0x08  // buffer completion interrupt status (IOC) (R/WC)
#define ICH_PO_SR_FIFO    0x10  // FIFO error interrupt (R/WC)

#define ICH_GLOB_CNT_REG       0x2c  // Global control register
#define ICH_GLOB_CNT_ACLINKOFF 0x00000008 // turn off ac97 link
#define ICH_GLOB_CNT_AC97WARM  0x00000004 // AC'97 warm reset
#define ICH_GLOB_CNT_AC97COLD  0x00000002 // AC'97 cold reset

#define ICH_PCM_20BIT      0x00400000 // 20-bit samples (ICH4)
#define ICH_PCM_246_MASK  0x00300000 // 6 channels (not all chips)

// from Linux 6.6-rc3, sound/pci/intel8x0.c
#define ICH_SIS_PCM_246_MASK  0x000000c0      /* 6 channels (SIS7012) */

#define ICH_GLOB_STAT_REG 0x30       // Global Status register (RO)
#define ICH_GLOB_STAT_PCR 0x00000100 // Primary codec is ready for action (software must check these bits before starting the codec!)
#define ICH_GLOB_STAT_RCS 0x00008000 // read completion status
#define ICH_SAMPLE_CAP      0x00c00000 // ICH4: sample capability bits (RO)
#define ICH_SAMPLE_16_20  0x00400000 // ICH4: 16- and 20-bit samples

#define ICH_PO_BDBAR_REG  0x10  // PCM out buffer descriptor BAR
#define ICH_PO_LVI_REG    0x15  // PCM out Last Valid Index (set it)
#define ICH_PO_CIV_REG    0x14  // PCM out current Index value (RO)

#define ICH_PO_PICB_REG   ((card->device_type==DEVICE_SIS)?0x16:0x18)  // PCM out position in current buffer(RO) (remaining, not processed pos)

#define ICH_ACC_SEMA_REG  0x34  // codec write semiphore register
#define ICH_CODEC_BUSY    0x01  // codec register I/O is happening self clearing

#define ICH_BD_IOC        0x8000 //buffer descriptor high word: interrupt on completion (IOC)

#define ICH_DMABUF_MAX_PERIODS  32 // number of entries in the Buffer Descriptor List
#define ICH_DMABUF_PERIODS   4 // number of "used" entries in the Buffer Descriptor List
#define ICH_BDL_ENTRY_SIZE (2 * sizeof(uint32_t))  // size of one entry in the Buffer Descriptor List
#define ICH_DMABUF_ALIGN (ICH_DMABUF_MAX_PERIODS*ICH_BDL_ENTRY_SIZE) // 256
#ifdef SBEMU
// XXX: This is unused
//#define ICH_INT_INTERVAL     1 //interrupt interval in periods //long interval won't work for doom/doom2
#endif

#define ICH_DEFAULT_RETRY 1000

typedef struct intel_card_s
{
 unsigned long   baseport_bm;       // busmaster baseport
 unsigned long   baseport_codec;    // mixer baseport
 unsigned int    irq;
 unsigned char   device_type;
 struct pci_config_s  *pci_dev;

 cardmem_t *dm;
 uint32_t *buffer_descriptor_list; // must be aligned to ICH_BDL_ENTRY_SIZE
 char *pcmout_buffer;
 long pcmout_bufsize;

 //unsigned int dma_size;
 unsigned int period_size_bytes;

 unsigned char vra;
 //unsigned char dra;
 unsigned int ac97_clock_detected;
 float ac97_clock_corrector;
}intel_card_s;

enum { DEVICE_INTEL, DEVICE_INTEL_ICH4, DEVICE_NFORCE, DEVICE_SIS };
static char *ich_devnames[4]={"ICH","ICH4","NForce","SIS7012"};

static void snd_intel_measure_ac97_clock(struct mpxplay_audioout_info_s *aui);

//-------------------------------------------------------------------------
// low level write & read

#define snd_intel_write_8(card,reg,data)  outb(card->baseport_bm+reg,data)
#define snd_intel_write_16(card,reg,data) outw(card->baseport_bm+reg,data)
#define snd_intel_write_32(card,reg,data) outl(card->baseport_bm+reg,data)

#define snd_intel_read_8(card,reg)  inb(card->baseport_bm+reg)
#define snd_intel_read_16(card,reg) inw(card->baseport_bm+reg)
#define snd_intel_read_32(card,reg) inl(card->baseport_bm+reg)

static unsigned int snd_intel_codec_ready(struct intel_card_s *card,unsigned int codec)
{
 unsigned int retry;

 if(!codec)
  codec=ICH_GLOB_STAT_PCR;

 // wait for codec ready status
 retry=ICH_DEFAULT_RETRY;
 do{
  if(snd_intel_read_32(card,ICH_GLOB_STAT_REG) & codec)
   break;
  pds_delay_10us(10);
 }while(--retry);
 return retry;
}

// compare: snd_intel8x0_codec_semaphore() in sound/pci/intel8x0.c
// always called with codec == ICH_GLOB_STAT_PCR (0x00000100)
static void snd_intel_codec_semaphore(struct intel_card_s *card,unsigned int codec)
{
 unsigned int retry;

 snd_intel_codec_ready(card,codec);

 //wait for semaphore ready (not busy) status
 retry=ICH_DEFAULT_RETRY;
 do{
  if(!(snd_intel_read_8(card,ICH_ACC_SEMA_REG)&ICH_CODEC_BUSY))
   return;
  pds_delay_10us(10);
 }while(--retry);

 mpxplay_debugf(ICH_DEBUG_OUTPUT,"semaphore timeout: %d",retry);

 // clear semaphore flag
 inw(card->baseport_codec); // (might be incompatible with ALI/ICH0?)
}

// compare snd_intel8x0_codec_write()
static void snd_intel_codec_write(struct intel_card_s *card,unsigned int reg,unsigned int data)
{
 snd_intel_codec_semaphore(card,ICH_GLOB_STAT_PCR);
 outw(card->baseport_codec+reg,data);
}

static unsigned int snd_intel_codec_read(struct intel_card_s *card,unsigned int reg)
{
 unsigned int data = 0,retry;
 snd_intel_codec_semaphore(card,ICH_GLOB_STAT_PCR);

 retry=ICH_DEFAULT_RETRY;
 do{
  data=inw(card->baseport_codec+reg);
  if(!(snd_intel_read_32(card,ICH_GLOB_STAT_REG)&ICH_GLOB_STAT_RCS))
   break;
  pds_delay_10us(10);
 }while(--retry);
 return data;
}

//-------------------------------------------------------------------------

static unsigned int snd_intel_buffer_init(struct intel_card_s *card,struct mpxplay_audioout_info_s *aui)
{
 unsigned int bytes_per_sample=(aui->bits_set>16)? 4:2;

 // TODO: Assume that this always results in 2 bytes per sample (16-bit)
 mpxplay_debugf(ICH_DEBUG_OUTPUT,"bytes per sample = %d (bits_set = %d)", bytes_per_sample, aui->bits_set);

 // TODO: For DEVICE_SIS, we might need to implement buffer size, as
 // the Linux kernel does (snd_intel8x0_pcm_open() in sound/pci/intel8x0.c)
 // if (chip->device_type == DEVICE_SIS) {
 //     runtime->hw.buffer_bytes_max = 64*1024;
 //     runtime->hw.period_bytes_max = 64*1024;
 // }

 card->pcmout_bufsize=MDma_get_max_pcmoutbufsize(aui,0,ICH_DMABUF_ALIGN,bytes_per_sample,0);
 // TODO: Check if card->pcmout_buffer size is <= 64*1024

 size_t buffer_descriptor_list_size = ICH_DMABUF_MAX_PERIODS*ICH_BDL_ENTRY_SIZE;

 // Allocate Buffer Descriptor List + PCM output buffer in a single allocation
 card->dm = MDma_alloc_cardmem(buffer_descriptor_list_size + card->pcmout_bufsize);

 // buffer descriptor list requires ICH_BDL_ENTRY_SIZE alignment,
 // but dos-allocmem gives 16 byte align (so we don't need alignment correction)
 card->buffer_descriptor_list = (uint32_t *)card->dm->linearptr;
 card->pcmout_buffer = card->dm->linearptr + buffer_descriptor_list_size;

 // DMA buffer written by MDma_writedata() and MDma_clearbuf()
 aui->card_DMABUFF = card->pcmout_buffer;

 // TODO: also check aui->card_dmasize <= 64*1024

 #ifdef SBEMU
 memset(card->pcmout_buffer, 0, card->pcmout_bufsize);
 #if 0
 // redirect buffer writes to dummy space
 cardmem_t *cm = MDma_alloc_cardmem(card->pcmout_bufsize);
 aui->card_DMABUFF = cm->linearptr;

 // keep playing sawtooth sound to see when DMA breaks
 for (int i=0; i<card->pcmout_bufsize; ++i) {
     card->pcmout_buffer[i] = i & 0xff;
 }
 #endif
 #endif

 mpxplay_debugf(ICH_DEBUG_OUTPUT,"buffer init: BDL:%8.8X pcmoutbuf:%8.8X size:%d",
         (unsigned long)card->buffer_descriptor_list,
         (unsigned long)card->pcmout_buffer,
         card->pcmout_bufsize);

 return 1;
}

static void snd_intel_chip_init(struct intel_card_s *card)
{
 unsigned int cmd,retry;

 cmd=snd_intel_read_32(card,ICH_GLOB_STAT_REG);
 mpxplay_debugf(ICH_DEBUG_OUTPUT,"clear status bits, current status =%x", cmd);

 // From Linux kernel

 /* put logic to right state */
 /* first clear status bits */
#define   ICH_MCINT             0x00000080      /* MIC capture interrupt */
#define   ICH_POINT             0x00000040      /* playback interrupt */
#define   ICH_PIINT             0x00000020      /* capture interrupt */

 snd_intel_write_32(card,ICH_GLOB_STAT_REG, cmd & (ICH_GLOB_STAT_RCS | ICH_MCINT | ICH_POINT | ICH_PIINT));

 mpxplay_debugf(ICH_DEBUG_OUTPUT,"ACLink ON, set 2 channels");
 cmd = snd_intel_read_32(card, ICH_GLOB_CNT_REG);
 if (card->device_type == DEVICE_SIS) {
     cmd&= ~(ICH_GLOB_CNT_ACLINKOFF | ICH_SIS_PCM_246_MASK);
 } else {
     cmd&= ~(ICH_GLOB_CNT_ACLINKOFF | ICH_PCM_246_MASK);
 }
 // finish cold or do warm reset
 cmd |= ((cmd&ICH_GLOB_CNT_AC97COLD)==0)? ICH_GLOB_CNT_AC97COLD : ICH_GLOB_CNT_AC97WARM;
 snd_intel_write_32(card, ICH_GLOB_CNT_REG, cmd);
 /*
 snd_intel_write_32(card, ICH_GLOB_CNT_REG, cmd & ~ICH_GLOB_CNT_AC97COLD);
 cmd = snd_intel_read_32(card, ICH_GLOB_CNT_REG);
 pds_delay_10us(10);
 snd_intel_write_32(card, ICH_GLOB_CNT_REG, cmd | ICH_GLOB_CNT_AC97COLD);
 pds_delay_10us(10);
 */
 mpxplay_debugf(ICH_DEBUG_OUTPUT,"AC97 reset type: %s",((cmd&ICH_GLOB_CNT_AC97COLD)? "cold":"warm"));

 retry=ICH_DEFAULT_RETRY;
 do{
  unsigned int cntreg=snd_intel_read_32(card,ICH_GLOB_CNT_REG);
  if(!(cntreg&ICH_GLOB_CNT_AC97WARM))
   break;
  pds_delay_10us(10);
 }while(--retry);
 mpxplay_debugf(ICH_DEBUG_OUTPUT,"AC97 reset timeout:%d",retry);

 // wait for primary codec ready status
 retry=snd_intel_codec_ready(card,ICH_GLOB_STAT_PCR);
 mpxplay_debugf(ICH_DEBUG_OUTPUT,"primary codec reset timeout:%d",retry);

 snd_intel_codec_read(card,0); // clear semaphore flag (might be incompatible with ALI/ICH0?)
 snd_intel_write_8(card,ICH_PO_CR_REG,ICH_PO_CR_RESET); // reset channels
 #ifdef SBEMU
 // Enable the IOC interrupt
 snd_intel_write_8(card,ICH_PO_CR_REG,ICH_PO_CR_LVBIE | ICH_PO_CR_IOCE | ICH_PO_CR_FEIE);
 #endif

 // code from sound/pci/intel8x0.c in linux-6.6-rc3
 if (card->device_type == DEVICE_SIS) {
     mpxplay_debugf(ICH_DEBUG_OUTPUT,"unmute the output on SIS7012");
     /* unmute the output on SIS7012 */
     mpxplay_debugf(ICH_DEBUG_OUTPUT,"SIS7012 register at 0x4c: %x", snd_intel_read_16(card, 0x4c));
     snd_intel_write_16(card, 0x4c, snd_intel_read_16(card, 0x4c) | 1);
     mpxplay_debugf(ICH_DEBUG_OUTPUT,"SIS7012 register at 0x4c later: %x", snd_intel_read_16(card, 0x4c));
 }

 mpxplay_debugf(ICH_DEBUG_OUTPUT,"chip init end");
}

static void snd_intel_chip_close(struct intel_card_s *card)
{
 if(card->baseport_bm)
  snd_intel_write_8(card,ICH_PO_CR_REG,ICH_PO_CR_RESET); // reset codec
}

static void snd_intel_ac97_init(struct intel_card_s *card,unsigned int freq_set)
{
 // initial ac97 volumes (and clear mute flag)
 snd_intel_codec_write(card, AC97_MASTER_VOL_STEREO, 0x0202);
 snd_intel_codec_write(card, AC97_PCMOUT_VOL,        0x0202);
 snd_intel_codec_write(card, AC97_HEADPHONE_VOL,     0x0202);

 // FIXME: what about this spdif thingie here?
 snd_intel_codec_write(card, AC97_EXTENDED_STATUS,AC97_EA_SPDIF);

#if 0
 // set/check variable bit rate bit
 if(freq_set!=48000){
  snd_intel_codec_write(card,AC97_EXTENDED_STATUS,AC97_EA_VRA);
  if(snd_intel_codec_read(card,AC97_EXTENDED_STATUS)&AC97_EA_VRA)
   card->vra=1;
 }
#endif
 mpxplay_debugf(ICH_DEBUG_OUTPUT,"ac97 init end (vra:%d)",card->vra);
}

static void snd_intel_prepare_playback(struct intel_card_s *card,struct mpxplay_audioout_info_s *aui)
{
 uint32_t *table_base;
 unsigned int i,cmd,retry,spdif_rate,period_size_samples;

 mpxplay_debugf(ICH_DEBUG_OUTPUT,"prepare playback: period_size_bytes:%d",card->period_size_bytes);
 // wait until DMA stopped ???
 retry=ICH_DEFAULT_RETRY;
 do{
  if(snd_intel_read_8(card,ICH_PO_SR_REG)&ICH_PO_SR_DCH)
   break;
  pds_delay_10us(1);
 }while(--retry);
 mpxplay_debugf(ICH_DEBUG_OUTPUT,"dma stop timeout: %d",retry);

 // reset codec
 snd_intel_write_8(card,ICH_PO_CR_REG, snd_intel_read_8(card, ICH_PO_CR_REG) | ICH_PO_CR_RESET);

 // Compare snd_intel8x0_setup_pcm_out() in Linux sound/pci/intel8x0.c
 if (card->device_type == DEVICE_SIS) {
     cmd = snd_intel_read_32(card, ICH_GLOB_CNT_REG);

     // force 2 channels by masking out the 4- and 6-channel bits (ICH_SIS_PCM_2 == 0)
     cmd &= ~ICH_SIS_PCM_246_MASK;

     snd_intel_write_32(card, ICH_GLOB_CNT_REG, cmd);
 } else {
     // set channels (2) and bits (16/32)
     cmd=snd_intel_read_32(card,ICH_GLOB_CNT_REG);
     funcbit_disable(cmd,(ICH_PCM_246_MASK | ICH_PCM_20BIT));
     if(aui->bits_set>16){
      if((card->device_type==DEVICE_INTEL_ICH4) && ((snd_intel_read_32(card,ICH_GLOB_STAT_REG)&ICH_SAMPLE_CAP)==ICH_SAMPLE_16_20)){
       aui->bits_card=32;
       funcbit_enable(cmd,ICH_PCM_20BIT);
      }
     }
     snd_intel_write_32(card,ICH_GLOB_CNT_REG,cmd);
 }

 // set spdif freq (???)
 switch(aui->freq_card){
  case 32000:spdif_rate=AC97_SC_SPSR_32K;break;
  case 44100:spdif_rate=AC97_SC_SPSR_44K;break;
  default:spdif_rate=AC97_SC_SPSR_48K;break;
 }
 cmd=snd_intel_codec_read(card,AC97_SPDIF_CONTROL);
 cmd&=AC97_SC_SPSR_MASK;
 cmd|=spdif_rate;
 snd_intel_codec_write(card,AC97_SPDIF_CONTROL,cmd);
 pds_delay_10us(10);

 //set analog ac97 freq
 mpxplay_debugf(ICH_DEBUG_OUTPUT,"AC97 front dac freq:%d ",aui->freq_card);
 if(card->ac97_clock_corrector){
  if(card->vra) {
   snd_intel_codec_write(card,AC97_PCM_FRONT_DAC_RATE,(long)((float)aui->freq_card*card->ac97_clock_corrector));
  } else {// !!! not good (AU_setrate will be called at every songs?) (maybe -of 48000 helps)
   aui->freq_card=(long)((float)aui->freq_card/card->ac97_clock_corrector);
   snd_intel_codec_write(card,AC97_PCM_FRONT_DAC_RATE,aui->freq_card);
  }
 }else {
  snd_intel_codec_write(card,AC97_PCM_FRONT_DAC_RATE,aui->freq_card);
 }

 pds_delay_10us(1600);

 // aka Buffer Descriptor list: https://wiki.osdev.org/AC97#Buffer_Descriptor_List
 //set period table
 table_base=card->buffer_descriptor_list;
 period_size_samples=card->period_size_bytes/(aui->bits_card>>3);
 for(i=0; i<ICH_DMABUF_PERIODS; i++){
  table_base[i*2]=(uint32_t)pds_cardmem_physicalptr(card->dm,(char *)card->pcmout_buffer+(i*card->period_size_bytes));
  /* From Linux kernel sources:
   * SIS7012 handles the pcm data in bytes, others are in samples */
  table_base[i*2+1]=((card->device_type==DEVICE_SIS)?card->period_size_bytes:period_size_samples);

  #ifdef SBEMU
  table_base[i*2+1] |= (ICH_BD_IOC<<16);
  #endif
 }

 // fill the rest of the entries in the BDL with null pointers, zero size, and no interrupt flags
 for(; i < ICH_DMABUF_MAX_PERIODS; ++i)
 {
  table_base[i*2]=0;
  table_base[i*2+1]=0;
 }

 snd_intel_write_32(card,ICH_PO_BDBAR_REG,(uint32_t)pds_cardmem_physicalptr(card->dm,table_base));

 snd_intel_write_8(card,ICH_PO_LVI_REG,(ICH_DMABUF_PERIODS-1)); // set last index

 // XXX: current index is read-only, so this probably doesn't work
 snd_intel_write_8(card,ICH_PO_CIV_REG,0); // reset current index

 // clear interrupts (linux does this, too)
 snd_intel_write_8(card, ICH_PO_SR_REG, ICH_PO_SR_LVBCI | ICH_PO_SR_BCIS | ICH_PO_SR_FIFO);

 mpxplay_debugf(ICH_DEBUG_OUTPUT,"prepare playback end");
 #ifdef SBEMU
 // FIXME: Does this have to be adjusted for SIS7012?
 aui->card_samples_per_int = period_size_samples / 2;
 #endif
}

//-------------------------------------------------------------------------
static pci_device_s ich_devices[]={
 {"82801AA",0x8086,0x2415, DEVICE_INTEL},
 {"82901AB",0x8086,0x2425, DEVICE_INTEL},
 {"82801BA",0x8086,0x2445, DEVICE_INTEL},
 {"ICH3"   ,0x8086,0x2485, DEVICE_INTEL},
 {"ICH4"   ,0x8086,0x24c5, DEVICE_INTEL_ICH4},
 {"ICH5"   ,0x8086,0x24d5, DEVICE_INTEL_ICH4},
 {"ESB"    ,0x8086,0x25a6, DEVICE_INTEL_ICH4},
 {"ICH6"   ,0x8086,0x266e, DEVICE_INTEL_ICH4},
 {"ICH7"   ,0x8086,0x27de, DEVICE_INTEL_ICH4},
 {"ESB2"   ,0x8086,0x2698, DEVICE_INTEL_ICH4},
 {"440MX"  ,0x8086,0x7195, DEVICE_INTEL}, // maybe doesn't work (needs extra pci hack)
 {"SI7012" ,0x1039,0x7012, DEVICE_SIS},
 {"NFORCE" ,0x10de,0x01b1, DEVICE_NFORCE},
 {"MCP04"  ,0x10de,0x003a, DEVICE_NFORCE},
 {"NFORCE2",0x10de,0x006a, DEVICE_NFORCE},
 {"CK804"  ,0x10de,0x0059, DEVICE_NFORCE},
 {"CK8"    ,0x10de,0x008a, DEVICE_NFORCE},
 {"NFORCE3",0x10de,0x00da, DEVICE_NFORCE},
 {"CK8S"   ,0x10de,0x00ea, DEVICE_NFORCE},
 {"AMD8111",0x1022,0x746d, DEVICE_INTEL},
 {"AMD768" ,0x1022,0x7445, DEVICE_INTEL},
 //{"ALI5455",0x10b9,0x5455, DEVICE_ALI}, // needs extra code
 {NULL,0,0,0}
};

static void INTELICH_close(struct mpxplay_audioout_info_s *aui);

static void INTELICH_card_info(struct mpxplay_audioout_info_s *aui)
{
 struct intel_card_s *card=aui->card_private_data;
 char sout[100];
 sprintf(sout,"ICH : Intel %s found on port:%4.4X irq:%d (type:%s, bits:16%s)",
         card->pci_dev->device_name,card->baseport_bm,card->irq,
         ich_devnames[card->device_type],((card->device_type==DEVICE_INTEL_ICH4)? ",20":""));
 pds_textdisplay_printf(sout);
}

static int INTELICH_adetect(struct mpxplay_audioout_info_s *aui)
{
 struct intel_card_s *card;

 card=(struct intel_card_s *)pds_calloc(1,sizeof(struct intel_card_s));
 if(!card)
  return 0;
 aui->card_private_data=card;

 card->pci_dev=(struct pci_config_s *)pds_calloc(1,sizeof(struct pci_config_s));
 if(!card->pci_dev)
  goto err_adetect;

 if(pcibios_search_devices(ich_devices,card->pci_dev)!=PCI_SUCCESSFUL)
  goto err_adetect;

#ifdef SBEMU
 if(card->pci_dev->device_type == DEVICE_INTEL_ICH4)
 { //enable leagcy IO space, must be set before setting PCI CMD's IO space bit.
  mpxplay_debugf(ICH_DEBUG_OUTPUT,"Eanble legacy io space for ICH4.\n");
  pcibios_WriteConfig_Byte(card->pci_dev, 0x41, 1); //IOSE:enable IO space
 }
 #endif

 mpxplay_debugf(ICH_DEBUG_OUTPUT,"chip init : enable PCI io and busmaster");
 pcibios_set_master(card->pci_dev);

 // osdev.org says: Before attempting to read the information about the BAR, make sure to disable both I/O and memory decode in the command byte
 // (this might just apply to trying to get the mapped memory size, though?)

 // TODO: Check if the type of the BAR is 16-bit (https://wiki.osdev.org/PCI#Base_Address_Registers)
 card->baseport_bm = pcibios_ReadConfig_Dword(card->pci_dev, PCIR_NABMBAR)&0xfff0;
 mpxplay_debugf(ICH_DEBUG_OUTPUT," BAR0 (native audio mixer base address): %x", card->baseport_bm);

 #ifdef SBEMU
 //some BIOSes don't set NAMBAR/NABMBAR at all. assign manually
 int iobase = 0xF000; //0xFFFF didn't work
 if(card->baseport_bm == 0)
 {
     iobase &=~0x3F;
     pcibios_WriteConfig_Dword(card->pci_dev, PCIR_NABMBAR, iobase);
     mpxplay_debugf(ICH_DEBUG_OUTPUT," native audio mixer base addr not set");
     card->baseport_bm = pcibios_ReadConfig_Dword(card->pci_dev, PCIR_NABMBAR)&0xfff0;
 }
 #endif

 if(!card->baseport_bm)
  goto err_adetect;
 // TODO: Check if the type of the BAR is 16-bit (https://wiki.osdev.org/PCI#Base_Address_Registers)
 card->baseport_codec = pcibios_ReadConfig_Dword(card->pci_dev, PCIR_NAMBAR)&0xfff0;
 mpxplay_debugf(ICH_DEBUG_OUTPUT," BAR1 (native audio bus mastering base address): %x", card->baseport_codec);

 #ifdef SBEMU
 if(card->baseport_codec == 0)
 {
    iobase -= 256;
    iobase &= ~0xFF;
    pcibios_WriteConfig_Dword(card->pci_dev, PCIR_NAMBAR, iobase);
    mpxplay_debugf(ICH_DEBUG_OUTPUT," native audio bus mastering base addr not set");
    card->baseport_codec = pcibios_ReadConfig_Dword(card->pci_dev, PCIR_NAMBAR)&0xfff0;
 }
 #endif
 if(!card->baseport_codec)
  goto err_adetect;
 aui->card_irq = card->irq = pcibios_ReadConfig_Byte(card->pci_dev, PCIR_INTR_LN);
 mpxplay_debugf(ICH_DEBUG_OUTPUT," interrupt pin: %d", aui->card_irq);
 #ifdef SBEMU
 if(aui->card_irq == 0xFF || aui->card_irq == 0)
 {
     pcibios_WriteConfig_Byte(card->pci_dev, PCIR_INTR_LN, 11);
     mpxplay_debugf(ICH_DEBUG_OUTPUT," no IRQ pin set, trying to force interrupt 11");
     aui->card_irq = card->irq = pcibios_ReadConfig_Byte(card->pci_dev, PCIR_INTR_LN);
 }
  #endif
 
 card->device_type=card->pci_dev->device_type;

 mpxplay_debugf(ICH_DEBUG_OUTPUT,"vend_id:%4.4X dev_id:%4.4X devtype:%s bmport:%4.4X mixport:%4.4X irq:%d",
  card->pci_dev->vendor_id,card->pci_dev->device_id,ich_devnames[card->device_type],card->baseport_bm,card->baseport_codec,card->irq);

 if(!snd_intel_buffer_init(card,aui))
  goto err_adetect;
 snd_intel_chip_init(card);
 snd_intel_ac97_init(card,aui->freq_set);
 return 1;

err_adetect:
 INTELICH_close(aui);
 return 0;
}

static void INTELICH_close(struct mpxplay_audioout_info_s *aui)
{
 struct intel_card_s *card=aui->card_private_data;
 if(card){
  snd_intel_chip_close(card);
  MDma_free_cardmem(card->dm);
  if(card->pci_dev)
   pds_free(card->pci_dev);
  pds_free(card);
  aui->card_private_data=NULL;
 }
}

static void INTELICH_setrate(struct mpxplay_audioout_info_s *aui)
{
 struct intel_card_s *card=aui->card_private_data;
 unsigned int dmabufsize;
 if((card->device_type==DEVICE_INTEL) && !card->ac97_clock_detected)
  snd_intel_measure_ac97_clock(aui); // called from here because pds_gettimeu() needs int08

 aui->card_wave_id=MPXPLAY_WAVEID_PCM_SLE;
 aui->chan_card=2;
 aui->bits_card=16;

 // FIXME: Maybe force this to 48000?

 if(!card->vra){
  aui->freq_card=48000;
 }else{
  if(aui->freq_card<8000)
   aui->freq_card=8000;
  else
   if(aui->freq_card>48000)
    aui->freq_card=48000;
 }

 mpxplay_debugf(ICH_DEBUG_OUTPUT, "aui->freq_card=%d\n", aui->freq_card);

 dmabufsize=MDma_init_pcmoutbuf(aui,card->pcmout_bufsize,ICH_DMABUF_ALIGN,0);
 // period size bytes... must be <= 0xFFFE * sample size (ref: https://wiki.osdev.org/AC97#Buffer_Descriptor_List)
 card->period_size_bytes=dmabufsize/ICH_DMABUF_PERIODS;

 if (card->device_type == DEVICE_SIS && card->period_size_bytes > 0xFFFE) {
     mpxplay_debugf(ICH_DEBUG_OUTPUT, "card->period_size_bytes = %x too big for SIS7012\n", card->period_size_bytes);
 }

 snd_intel_prepare_playback(card,aui);
}

static void INTELICH_start(struct mpxplay_audioout_info_s *aui)
{
 struct intel_card_s *card=aui->card_private_data;
 unsigned char cmd;

 snd_intel_codec_ready(card,ICH_GLOB_STAT_PCR);

 #ifdef SBEMU
 // This kicks off the playback of the buffers
 snd_intel_write_8(card,ICH_PO_CR_REG, snd_intel_read_8(card,ICH_PO_CR_REG) |
         ICH_PO_CR_LVBIE | ICH_PO_CR_IOCE | ICH_PO_CR_START);
 //snd_intel_write_8(card,ICH_PO_LVI_REG,(snd_intel_read_8(card, ICH_PO_CIV_REG)-1)%ICH_DMABUF_PERIODS);
 #endif
}

static void INTELICH_stop(struct mpxplay_audioout_info_s *aui)
{
 struct intel_card_s *card=aui->card_private_data;
 unsigned char cmd;

 // this stops playback of the buffers (clear ICH_PO_CR_START flag)
 cmd=snd_intel_read_8(card,ICH_PO_CR_REG);
 funcbit_disable(cmd,ICH_PO_CR_START);
 snd_intel_write_8(card,ICH_PO_CR_REG,cmd);
}

static void snd_intel_measure_ac97_clock(struct mpxplay_audioout_info_s *aui)
{
 struct intel_card_s *card=aui->card_private_data;
 mpxp_int64_t starttime,endtime,timelen; // in usecs
 long freq_save=aui->freq_card,dmabufsize;

 aui->freq_card=48000;
 aui->chan_card=2;
 aui->bits_card=16;

 dmabufsize=min(card->pcmout_bufsize,AUCARDS_DMABUFSIZE_NORMAL); // to avoid longer test at -ddma, -ob 24
 dmabufsize=MDma_init_pcmoutbuf(aui,dmabufsize,ICH_DMABUF_ALIGN,0);
 card->period_size_bytes=dmabufsize/ICH_DMABUF_PERIODS;
 snd_intel_prepare_playback(card,aui);
 MDma_clearbuf(aui);

#ifdef SBEMU
int cr = snd_intel_read_8(card,ICH_PO_CR_REG);
snd_intel_write_8(card,ICH_PO_CR_REG, 0); //disable LVBIE/IOCE
#endif
 INTELICH_start(aui);
 starttime=pds_gettimeu();
 do{
  if(snd_intel_read_8(card,ICH_PO_CIV_REG)>=(ICH_DMABUF_PERIODS-1)) // current index has reached last index
   if(snd_intel_read_8(card,ICH_PO_CIV_REG)>=(ICH_DMABUF_PERIODS-1)) // verifying
    break;
 }while(pds_gettimeu()<=(starttime+1000000)); // abort after 1 sec (btw. the test should run less than 0.2 sec only)
 endtime=pds_gettimeu();
 if(endtime>starttime)
  timelen=endtime-starttime;
 else
  timelen=0;
 INTELICH_stop(aui);
 #ifdef SBEMU
 snd_intel_write_8(card,ICH_PO_CR_REG, cr);
 #endif

 if(timelen && (timelen<1000000)){
  dmabufsize=card->period_size_bytes*(ICH_DMABUF_PERIODS-1); // the test buflen
  card->ac97_clock_corrector=
    ((float)aui->freq_card*aui->chan_card*(aui->bits_card/8)) // dataspeed (have to be)
   /((float)dmabufsize*1000000.0/(float)timelen);             // sentspeed (the measured) (bytes/sec)
  if((card->ac97_clock_corrector>0.99) && (card->ac97_clock_corrector<1.01)) // dataspeed==sentspeed
   card->ac97_clock_corrector=0.0;
  if((card->ac97_clock_corrector<0.60) || (card->ac97_clock_corrector>1.5)) // we assume that the result is false
   card->ac97_clock_corrector=0.0;
 }
 aui->freq_card=freq_save;
 card->ac97_clock_detected=1;
 mpxplay_debugf(ICH_DEBUG_OUTPUT,"ac97_clock_corrector: %1.4f timelen:%d us",card->ac97_clock_corrector,(long)timelen);
}

//------------------------------------------------------------------------

static void INTELICH_writedata(struct mpxplay_audioout_info_s *aui,char *src,unsigned long left)
{
 struct intel_card_s *card=aui->card_private_data;
 unsigned int index;

 MDma_writedata(aui,src,left);

#if 0
 #ifdef SBEMU
 // FIXME -- the Last Valid Buffer Entry should probably be set to how much we have mixed?
 snd_intel_write_8(card,ICH_PO_LVI_REG,(snd_intel_read_8(card, ICH_PO_CIV_REG)-1)%ICH_DMABUF_PERIODS);
 #else
 index=aui->card_dmalastput/card->period_size_bytes;
 snd_intel_write_8(card,ICH_PO_LVI_REG,(index-1)%ICH_DMABUF_PERIODS); // set stop position (to keep playing in an endless loop)
 #endif
#endif
 //mpxplay_debugf(ICH_DEBUG_OUTPUT,"put-index: %d",index);
}

static long INTELICH_getbufpos(struct mpxplay_audioout_info_s *aui)
{
 struct intel_card_s *card=aui->card_private_data;
 unsigned long bufpos=0;
 unsigned int index,pcmpos,retry=3;

 do{
  index=snd_intel_read_8(card,ICH_PO_CIV_REG);    // number of current period
  #ifndef SBEMU
  //mpxplay_debugf(ICH_DEBUG_OUTPUT,"index1: %d",index);
  if(index>=ICH_DMABUF_PERIODS){
   if(retry>1)
    continue;
   MDma_clearbuf(aui);
   //snd_intel_write_8(card,ICH_PO_LVI_REG,(ICH_DMABUF_PERIODS-1));
   // FIXME: CIV is read-only, this cannot be set
   snd_intel_write_8(card,ICH_PO_CIV_REG,0);
   funcbit_enable(aui->card_infobits,AUINFOS_CARDINFOBIT_DMAUNDERRUN);
   continue;
  }
  #endif

  pcmpos=snd_intel_read_16(card,ICH_PO_PICB_REG); // position in the current period (remaining unprocessed in SAMPLEs)
  if (card->device_type != DEVICE_SIS) {
      // Convert number of samples to number of bytes (but not for SIS7012)
      pcmpos *= aui->bits_card>>3;
  }
  //pcmpos*=aui->chan_card;
  //printf("%d %d %d %d\n",aui->bits_card, aui->chan_card, pcmpos, card->period_size_bytes);
  //mpxplay_debugf(ICH_DEBUG_OUTPUT,"pcmpos: %d",pcmpos);
  if(!pcmpos || pcmpos > card->period_size_bytes){
   if(snd_intel_read_8(card,ICH_PO_LVI_REG)==index){
    MDma_clearbuf(aui);
    //snd_intel_write_8(card,ICH_PO_LVI_REG,(index-1)%ICH_DMABUF_PERIODS); // to keep playing in an endless loop
    //snd_intel_write_8(card,ICH_PO_CIV_REG,index); // ??? -RO
    funcbit_enable(aui->card_infobits,AUINFOS_CARDINFOBIT_DMAUNDERRUN);
   }
   #ifndef SBEMU
   continue;
   #endif
  }
  #ifndef SBEMU
  if(snd_intel_read_8(card,ICH_PO_CIV_REG)!=index) // verifying
   continue;
  #endif

  pcmpos=card->period_size_bytes-pcmpos;
  bufpos=index*card->period_size_bytes+pcmpos;

  if(bufpos<aui->card_dmasize){
   aui->card_dma_lastgoodpos=bufpos;
   break;
  }

 }while(--retry);

 //mpxplay_debugf(ICH_DEBUG_OUTPUT,"bufpos:%5d dmasize:%5d",bufpos,aui->card_dmasize);

 return aui->card_dma_lastgoodpos;
}

//--------------------------------------------------------------------------
//mixer

static void INTELICH_writeMIXER(struct mpxplay_audioout_info_s *aui,unsigned long reg, unsigned long val)
{
 struct intel_card_s *card=aui->card_private_data;
 snd_intel_codec_write(card,reg,val);
}

static unsigned long INTELICH_readMIXER(struct mpxplay_audioout_info_s *aui,unsigned long reg)
{
 struct intel_card_s *card=aui->card_private_data;
 return snd_intel_codec_read(card,reg);
}

static int
g_counts[SIS7012_DEBUG_LAST];

int sis7012_get(enum SIS7012_DebugVars var)
{
    return g_counts[var];
}

#ifdef SBEMU
static int INTELICH_IRQRoutine(mpxplay_audioout_info_s* aui)
{
  intel_card_s *card=aui->card_private_data;

  // TODO: Maybe clear resume interrupt (like FreeBSD)?

  int status = snd_intel_read_8(card,ICH_PO_SR_REG);

  // Could handle interrupts here
  if (status & ICH_PO_SR_LVBCI) {
      // Last Valid Buffer Completion -- this seems to signify DMA underrun
      g_counts[SIS7012_DEBUG_BUP_COUNT]++;

      // This kicks off the playback of the buffers (again)
      snd_intel_write_8(card,ICH_PO_CR_REG, snd_intel_read_8(card, ICH_PO_CR_REG) |
              ICH_PO_CR_START | ICH_PO_CR_IOCE | ICH_PO_CR_FEIE | ICH_PO_CR_LVBIE);

      // TODO: does this improve things?
      snd_intel_write_8(card,ICH_PO_LVI_REG,(ICH_DMABUF_PERIODS-1)); // set last index
  }

  if (status & ICH_PO_SR_BCIS) {
      // Buffer Completion Interrupt Status (aka IOC, when the high bit is set in the BDL size field)
      g_counts[SIS7012_DEBUG_IOC_COUNT]++;

      // to keep playing in an endless loop
      snd_intel_write_8(card, ICH_PO_LVI_REG, (snd_intel_read_8(card,ICH_PO_LVI_REG) + 1) % ICH_DMABUF_PERIODS);
  }

  if (status & ICH_PO_SR_FIFO) {
      // Linux does not handle this and just clears the interrupt
      g_counts[SIS7012_DEBUG_FIFO_COUNT]++;
  }

  // TODO: FreeBSD handles (status & (ICH_PO_SR_LVBCI | ICH_PO_SR_BCIS)) as incrementing the current buffer

  // acknowledge the interrupt we have seen
  snd_intel_write_8(card, ICH_PO_SR_REG, status & (ICH_PO_SR_LVBCI | ICH_PO_SR_BCIS | ICH_PO_SR_FIFO));

  return status != 0;
}
#endif

one_sndcard_info ICH_sndcard_info={
 "ICH AC97",
 SNDCARD_LOWLEVELHAND|SNDCARD_INT08_ALLOWED,

 NULL,
 NULL,                   // no init
 &INTELICH_adetect,      // only autodetect
 &INTELICH_card_info,
 &INTELICH_start,
 &INTELICH_stop,
 &INTELICH_close,
 &INTELICH_setrate,

 &INTELICH_writedata,
 &INTELICH_getbufpos,
 &MDma_clearbuf,
 NULL, // ICH doesn't need dma-monitor (LVI handles it)
 #ifdef SBEMU
 &INTELICH_IRQRoutine,
 #else
 NULL,
 #endif

 &INTELICH_writeMIXER,
 &INTELICH_readMIXER,
 &mpxplay_aucards_ac97chan_mixerset[0]
};

#endif
