/*
    By Sirus Shahini
    ~cyn
*/

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/mm_types.h>
#include <linux/page-flags.h>
#include <linux/mmzone.h>
#include <linux/gfp.h>
#include <linux/pfn.h>
#include <linux/hugetlb.h>
#include <linux/syscalls.h>
#include <asm/cacheflush.h>
#include <asm/uaccess.h>
#include <linux/utsname.h>
#include <linux/moduleparam.h>
#include <linux/cryptohash.h>
#include <linux/siphash.h>
#include <linux/random.h>


siphash_key_t seq_secret;
siphash_key_t last_secret;

unsigned long tcp_secure_seq_adr;
u8 p_bits;
u8 backup_bytes[12];

#define CNORM				"\x1b[0m"
#define CRED				"\x1b[1;31m"
#define CGREEN				"\x1b[1;32m"


void _s_out(u8 err, char *fmt, ...){
    va_list argp;
    char msg_fmt[255];


    if (err){
		strcpy(msg_fmt,CRED"[!] "CNORM);
    }else{
		strcpy(msg_fmt,CGREEN"[-] "CNORM);
    }
    strcat(msg_fmt,fmt);
    strcat(msg_fmt,"\n");
    va_start(argp,fmt);
    vprintk(msg_fmt,argp);
    va_end(argp);
}

u32 secure_tcp_seq_hooked(__be32 saddr, __be32 daddr,
		   __be16 sport, __be16 dport)
{
	u32 hash;
	u32 temp;

	temp = *((u32*)(&seq_secret.key[0]));
	temp>>=8;
	last_secret.key[0] += temp;
	temp = *((u32*)(&seq_secret.key[1]));
	temp>>=8;
	last_secret.key[1] += temp;

	hash = siphash_3u32((__force u32)saddr, (__force u32)daddr,
			        (__force u32)sport << 16 | (__force u32)dport,
			        &last_secret);
	return hash;
}
int store_p_bits(unsigned long address, unsigned char bits){
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *ptep;
	p4d_t *p4d;
	unsigned long ent_val;
	struct mm_struct *mm;

	unsigned short ps = 1 << 7;
	u8 cbit;
	u8 op_num;

	mm = current->mm;
	pgd = pgd_offset(mm, address);

	if (pgd_none(*pgd) || unlikely(pgd_bad(*pgd))){
		return -1;
	}
	ent_val = *((unsigned long*)pgd);
	op_num = 1;
	cbit = bits & op_num;
	if (cbit){
	    ent_val = ent_val | 2;
	}else{
	    ent_val = ent_val & ~((u8)2);
	}
	*((unsigned long*)pgd) = ent_val;

	p4d = p4d_offset(pgd,address);
	pud = pud_offset(p4d, address);

	ent_val = *((unsigned long*)pud);
	op_num = 2;
	cbit = bits & op_num;
	if (cbit){
	    ent_val = ent_val | 2;
	}else{
	    ent_val = ent_val & ~((u8)2);
	}
	*((unsigned long*)pud) = ent_val;
	if (!!( ps & *((unsigned long*)pud) ) == 1){
	    return 1;
	}
	pmd = pmd_offset(pud, address);
	/*
	 *	We don't have to check for this
	 *	but if this macro triggers a bug
	 *	here there's already something wrong
	 *	with mappings.
	 *	I leave it to stay here for the
	 *	sake of completeness.
	*/
	VM_BUG_ON(pmd_trans_huge(*pmd));
	ent_val = *((unsigned long*)pmd);

	op_num = 4;
	cbit = bits & op_num;
	if (cbit){
	    ent_val = ent_val | 2;
	}else{
	    ent_val = ent_val & ~((u8)2);
	}
	*((unsigned long*)pmd) = ent_val;
	if (!!( ps & *((unsigned long*)pmd) ) == 1){
	    return 1;
	}
	ptep=pte_offset_map(pmd, address);
	if (!ptep){
		return -1;
	}
	ent_val = *((unsigned long*)(ptep));
	op_num = 8;
	cbit = bits & op_num;
	if (cbit){
	    ent_val = ent_val | 2;
	}else{
	    ent_val = ent_val & ~((u8)2);
	}
	*((unsigned long*)ptep) = ent_val;
	return 1;
}


int hook_init(void){
	char payload[] = "\x48\xB8\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xE0";
	u8* payload_adr;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *ptep;
	unsigned long ent_val;
	struct mm_struct *mm;
	unsigned short ps = 1 << 7;
	u8 cbit;
	int i;

	tcp_secure_seq_adr = 0;
	memset(&seq_secret.key,0,32);
	/*
	 *	Find our function of interest and
	 *	read some random bytes
	*/
	tcp_secure_seq_adr = kallsyms_lookup_name("secure_tcp_seq");

	if (!tcp_secure_seq_adr){
		_s_out(1,"FATAL: Name lookup failed.");
		return -1; //EPERM but we use it a as generic error number
	}

	if (wait_for_random_bytes()){
		_s_out(1,"FATAL: Can't get random bytes form kernel.");
		return -1;
	}

	get_random_bytes(&seq_secret.key,32);

	for (i=0;i<32;i++){
		if ( *( ((u8*)(&seq_secret.key)) + i ) !=0)
			break;
	}

	if (i==32){
		_s_out(1,"FATAL: Random bytes are not valid.");
		return -1;
	}

	memcpy(&last_secret,&seq_secret,sizeof(seq_secret));

	/*
	 *	Ok, initialization must have succeeded.
	 *	Prepare the page tables and install the hook
	*/

	p_bits=0;

	mm = current->mm;
	pgd = pgd_offset(mm, tcp_secure_seq_adr);

	if (pgd_none(*pgd) || unlikely(pgd_bad(*pgd)))
		return -1;

	ent_val = *((unsigned long*)pgd);
	cbit = ent_val & 2;
	if (cbit) p_bits = 1;
	p4d = p4d_offset(pgd,tcp_secure_seq_adr);

	pud = pud_offset(p4d, tcp_secure_seq_adr);
	ent_val = *((unsigned long*)pud);
	cbit = ent_val & 2;
	if (cbit) p_bits = p_bits | 2;

	if (!!( ps & *((unsigned long*)pud) ) == 1){
	    goto install;
	}

	pmd = pmd_offset(pud, tcp_secure_seq_adr);
	VM_BUG_ON(pmd_trans_huge(*pmd));
	ent_val = *((unsigned long*)pmd);
	cbit = ent_val & 2;
	if (cbit) p_bits = p_bits | 4;

	if (!!( ps & *((unsigned long*)pmd) ) == 1){
	    goto install;
	}

	ptep=pte_offset_map(pmd, tcp_secure_seq_adr);

	if (!ptep){
		return -1;
	}

	ent_val = *((unsigned long*)(ptep));
	cbit = ent_val & 2;
	if (cbit) p_bits = p_bits | 8;



install:

	store_p_bits(tcp_secure_seq_adr,0x0F);

	payload_adr = (u8*) tcp_secure_seq_adr;
	memcpy(backup_bytes,(void*)tcp_secure_seq_adr,12);
	memcpy((void*)tcp_secure_seq_adr,payload,12);
	*((unsigned long*)&payload_adr[2]) = (unsigned long)&secure_tcp_seq_hooked;

	store_p_bits(tcp_secure_seq_adr,p_bits);

	_s_out(0,"Installing tirdad hook succeeded.");

	return 0;
}

void hook_exit(void){
	store_p_bits(tcp_secure_seq_adr,0x0F);
	memcpy((void*)tcp_secure_seq_adr,backup_bytes,12);
	store_p_bits(tcp_secure_seq_adr,p_bits);

	_s_out(0,"Removed tirdad hook successfully.");
}
module_init(hook_init);
module_exit(hook_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sirus Shahini <sirus.shahini@gmail.com>");
MODULE_DESCRIPTION("Tirdad hook for TCP ISN generator");
