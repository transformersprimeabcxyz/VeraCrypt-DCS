/** @file
This is DCS configuration, volume crypt

Copyright (c) 2016. Disk Cryptography Services for EFI (DCS), Alex Kolotnikov, Alex Kolotnikov
Copyright (c) 2016. VeraCrypt, Mounir IDRASSI 

This program and the accompanying materials
are licensed and made available under the terms and conditions
of the GNU Lesser General Public License, version 3.0 (LGPL-3.0).

The full text of the license may be found at
https://opensource.org/licenses/LGPL-3.0
**/

#include <Library/UefiBootServicesTableLib.h>
#include <Library/ShellLib.h>
#include <Library/DevicePathLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Guid/Gpt.h>
#include <Guid/GlobalVariable.h>

#include <Library/CommonLib.h>
#include <Library/GraphLib.h>
#include <Library/PasswordLib.h>
#include <Library/DcsCfgLib.h>

#include "common/Tcdefs.h"
#include "common/Endian.h"
#include "common/Crypto.h"
#include "common/Volumes.h"
#include "common/Crc.h"
#include "crypto/cpu.h"
#include "DcsVeraCrypt.h"
#include "BootCommon.h"

#include "DcsCfg.h"

PCRYPTO_INFO gAuthCryptInfo = NULL;
PCRYPTO_INFO gHeaderCryptInfo = NULL;
CHAR8 Header[512];

EFI_HANDLE              SecRegionHandle = NULL;
UINT64                  SecRegionSector = 0;
UINT8*                  SecRegionData = NULL;
UINTN                   SecRegionSize = 0;
UINTN                   SecRegionOffset = 0;
PCRYPTO_INFO            SecRegionCryptInfo = NULL;

//////////////////////////////////////////////////////////////////////////
// Crypt helpers
//////////////////////////////////////////////////////////////////////////
int
AskEA() {
	int ea;
	CHAR16 name[128];
	for (ea = EAGetFirst(); ea != 0; ea = EAGetNext(ea))
	{
		EAGetName(name, ea, 1);
		OUT_PRINT(L"(%d) %s\n", ea, name);
	}
	ea = (int)AskUINTN(":", EAGetFirst());
	return ea;
}

int
AskMode(int ea) {
	int mode;
	for (mode = EAGetFirstMode(ea); mode != 0; mode = EAGetNextMode(ea, mode))
	{
		EAGetModeName(ea, mode, 1);
		OUT_PRINT(L"(%d) %s\n", mode, EAGetModeName(ea, mode, 1));
	}
	mode = (int)AskUINTN(":", EAGetFirstMode(ea));
	return mode;
}

int
AskPkcs5() {
	int pkcs5 = 1;
	Hash *hash;
	hash = HashGet(pkcs5);
	while (hash != NULL)
	{
		OUT_PRINT(L"(%d) %s\n", pkcs5, hash->Name);
		++pkcs5;
		hash = HashGet(pkcs5);
	};
	pkcs5 = (int)AskUINTN(":", gAuthHash);
	return pkcs5;
}

EFI_STATUS
TryHeaderDecrypt(
	IN  CHAR8*                  header,
	OUT PCRYPTO_INFO            *rci,
	OUT PCRYPTO_INFO            *rhci
	) 
{
	int                 vcres;
	PCRYPTO_INFO        cryptoInfo;
	PCRYPTO_INFO        headerCryptoInfo = NULL;

	if (rhci != NULL) {
		headerCryptoInfo = crypto_open();
	}

	vcres = ReadVolumeHeader(
		gAuthBoot,
		header,
		&gAuthPassword,
		gAuthHash,
		gAuthPim,
		gAuthTc,
		&cryptoInfo,
		headerCryptoInfo);

	if (vcres != 0) {
		ERR_PRINT(L"Authorization failed. Wrong password, PIM or hash. Decrypt error(%x)\n", vcres);
		return EFI_INVALID_PARAMETER;
	}
	OUT_PRINT(L"%H" L"Success\n" L"%N", vcres);
	OUT_PRINT(L"Start %lld length %lld\nVolumeSize %lld\nhiddenVolumeSize %lld\nflags 0x%x\n",
		cryptoInfo->EncryptedAreaStart.Value, (uint64)cryptoInfo->EncryptedAreaLength.Value,
		cryptoInfo->VolumeSize,
		cryptoInfo->HeaderFlags
		);
	if(rci != NULL) *rci = cryptoInfo;
	if (rhci != NULL) *rhci = headerCryptoInfo;
	return EFI_SUCCESS;
}

EFI_STATUS
ChangePassword(
	IN OUT CHAR8*                  header
	)
{
	Password                newPassword;
	Password                confirmPassword;
	EFI_STATUS              res;
	PCRYPTO_INFO            cryptoInfo, ci;
	int                     vcres;

	res = RndPreapare();
	if (EFI_ERROR(res)) {
		ERR_PRINT(L"Rnd: %r\n", res);
		return res;
	}

	if (gAuthPasswordMsg == NULL) {
		VCAuthAsk();
	}

	res = TryHeaderDecrypt(header, &cryptoInfo, NULL);
	if (EFI_ERROR(res)) return res;

	if (AskConfirm("Change pwd[N]?", 1)) {
		return EFI_INVALID_PARAMETER;
	}

	do {
		ZeroMem(&newPassword, sizeof(newPassword));
		ZeroMem(&confirmPassword, sizeof(newPassword));
		VCAskPwd(AskPwdNew, &newPassword);
		VCAskPwd(AskPwdConfirm, &confirmPassword);
		if (newPassword.Length == confirmPassword.Length) {
			if (CompareMem(newPassword.Text, confirmPassword.Text, confirmPassword.Length) == 0) {
				break;
			}
		}
		if (AskConfirm("Password mismatch, retry?", 1)) {
			return EFI_INVALID_PARAMETER;
		}
	} while (TRUE);

	vcres = CreateVolumeHeaderInMemory(
		gAuthBoot, header,
		cryptoInfo->ea,
		cryptoInfo->mode,
		&newPassword,
		cryptoInfo->pkcs5,
		gAuthPim,
		cryptoInfo->master_keydata,
		&ci,
		cryptoInfo->VolumeSize.Value,
		cryptoInfo->hiddenVolumeSize,
		cryptoInfo->EncryptedAreaStart.Value,
		cryptoInfo->EncryptedAreaLength.Value,
		gAuthTc ? 0 : cryptoInfo->RequiredProgramVersion,
		cryptoInfo->HeaderFlags,
		cryptoInfo->SectorSize,
		FALSE);

	if (vcres != 0) {
		ERR_PRINT(L"header create error(%x)\n", vcres);
		return EFI_INVALID_PARAMETER;
	}
	return EFI_SUCCESS;
}

EFI_STATUS
CreateVolumeHeader(
	IN OUT CHAR8*                  header,
	OUT PCRYPTO_INFO               *rci,
	IN UINT64                      defaultVS,
	IN UINT64                      defaultESS,
	IN UINT64                      defaultESE
	)
{
	INT32                   vcres;
	int mode = 0;
	int ea = 0;
	int pkcs5 = 0;
	UINT64 encSectorStart = defaultESS;
	UINT64 encSectorEnd = defaultESE;
	UINT64 hiddenVolumeSize = 0;
	UINT64 VolumeSize = defaultVS;
	UINT32 HeaderFlags = 0;
	int8 master_keydata[MASTER_KEYDATA_SIZE];

	if (!RandgetBytes(master_keydata, MASTER_KEYDATA_SIZE, FALSE)) {
		ERR_PRINT(L"No randoms\n");
		return EFI_CRC_ERROR;
	}

	if (gAuthPasswordMsg == NULL) {
		VCAuthAsk();
	}

	ea = AskEA();
	mode = AskMode(ea);
	pkcs5 = AskPkcs5();
	encSectorStart = AskUINT64("encryption start (sector):", encSectorStart);
	encSectorEnd = AskUINT64("encryption end (sector):", encSectorEnd);
	VolumeSize = AskUINT64("volume total (sectors):", VolumeSize);
	hiddenVolumeSize = AskUINT64("hidden volume total (sectors):", hiddenVolumeSize);
	gAuthBoot = AskConfirm("Boot mode[N]?", 1);
	HeaderFlags = (UINT32)AskUINTN("flags:", gAuthBoot ? TC_HEADER_FLAG_ENCRYPTED_SYSTEM : 0);

	vcres = CreateVolumeHeaderInMemory(
		gAuthBoot, Header,
		ea,
		mode,
		&gAuthPassword,
		pkcs5,
		gAuthPim,
		master_keydata,
		rci,
		VolumeSize << 9,
		hiddenVolumeSize << 9,
		encSectorStart << 9,
		(encSectorEnd - encSectorStart + 1) << 9,
		VERSION_NUM,
		HeaderFlags,
		512,
		FALSE);

	if (vcres != 0) {
		ERR_PRINT(L"Header error %d\n", vcres);
		return EFI_CRC_ERROR;
	}
	return EFI_SUCCESS;
}

UINT8
AskChoice(
	CHAR8* prompt, 
	CHAR8* choice, 
	UINT8 visible) {
	CHAR16      buf[2];
	UINTN       len = 0;
	UINT8       ret = 0;
	UINT8       *pos = choice;
	while (ret == 0) {
		pos = choice;
		OUT_PRINT(L"%a", prompt);
		GetLine(&len, buf, NULL, sizeof(buf) / 2, visible);
		while (*pos != 0 && ret == 0) {
			if (buf[0] == *pos) {
				ret = *pos;
				break;
			}
			pos++;
		}
	}
	return ret;
}

UINT8
AskARI() {
	return AskChoice("[a]bort [r]etry [i]gnore?", "aArRiI", 1);
}

UINTN gScndTotal = 0;
UINTN gScndCurrent = 0;
VOID
AddSecondsDelta() 
{
	EFI_STATUS res;
	EFI_TIME time;
	UINTN secs;
	UINTN secsDelta;
	res = gST->RuntimeServices->GetTime(&time, NULL);
	if (EFI_ERROR(res)) return;
	secs = (UINTN)time.Second + ((UINTN)time.Minute) * 60 + ((UINTN)time.Hour) * 60 * 60;
	if (gScndTotal == 0 && gScndCurrent == 0) {
		gScndCurrent = secs;
		return;
	}
	if (secs > gScndCurrent) {
		secsDelta = secs - gScndCurrent;
	}	else {
		secsDelta = 24 * 60 * 60 - gScndCurrent;
		secsDelta += secs;
	}
	gScndCurrent = secs;
	gScndTotal += secsDelta;
}

VOID
RangeCryptProgress(
	IN UINT64  size,
	IN UINT64  remains,
	IN UINT64  pos,
	IN UINT64  remainsOnStart
	) {
	UINTN  percent;
	percent = (UINTN)(100 * (size - remains) / size);
	OUT_PRINT(L"%H%d%%%N (%llds %llds) ", percent, pos, remains);
	AddSecondsDelta();
	if (gScndTotal > 10) {
		UINT64 doneBpS = (remainsOnStart - remains) * 512 / gScndTotal;
		if (doneBpS > 1024 * 1024) {
			OUT_PRINT(L"%lldMB/s", doneBpS / (1024 * 1024));
		}	else	if (doneBpS > 1024) {
			OUT_PRINT(L"%lldKB/s", doneBpS / 1024);
		}	else {
			OUT_PRINT(L"%lldB/s", doneBpS);
		}
		if (doneBpS > 0) {
			OUT_PRINT(L"(ETA: %lldm)", (remains * 512 / doneBpS) / 60);
		}
	}
	OUT_PRINT(L"        \r");
}

#define CRYPT_BUF_SECTORS 50*1024*2
EFI_STATUS
RangeCrypt(
	IN EFI_HANDLE             disk,
	IN UINT64                 start,
	IN UINT64                 size,
	IN UINT64                 enSize,
	IN PCRYPTO_INFO           info,
	IN BOOL                   encrypt,
	IN PCRYPTO_INFO           headerInfo,
	IN UINT64                 headerSector
	)
{
	EFI_STATUS              res;
	EFI_BLOCK_IO_PROTOCOL  *io;
	UINT8*                  buf;
	UINT64                  remains;
	UINT64                  remainsOnStart;
	UINT64                  pos;
	UINTN                   rd;

	io = EfiGetBlockIO(disk);
	if (!io) {
		ERR_PRINT(L"no block IO\n");
		return EFI_INVALID_PARAMETER;
	}

	buf = MEM_ALLOC(CRYPT_BUF_SECTORS << 9);
	if (!buf) {
		ERR_PRINT(L"no memory for buffer\n");
		return EFI_INVALID_PARAMETER;
	}

	if (encrypt) {
		remains = size - enSize;
		pos = start + enSize;
	}	else {
		remains = enSize;
		rd = (UINTN)((remains > CRYPT_BUF_SECTORS) ? CRYPT_BUF_SECTORS : remains);
		pos = start + enSize - rd;
	}
	remainsOnStart = remains;
	// Start second
	gScndTotal = 0;
	gScndCurrent = 0;
	do {
		rd = (UINTN)((remains > CRYPT_BUF_SECTORS) ? CRYPT_BUF_SECTORS : remains);
		RangeCryptProgress(size, remains, pos, remainsOnStart);
		// Read
		do {
			res = io->ReadBlocks(io, io->Media->MediaId, pos, rd << 9, buf);
			if (EFI_ERROR(res)) {
				UINT8 ari;
				ERR_PRINT(L"Read error: %r\n", res);
				ari = AskARI();
				switch (ari)
				{
				case 'I':
				case 'i':
					res = EFI_SUCCESS;
					break;
				case 'A':
				case 'a':
					goto error;
				case 'R':
				case 'r':
				default:
					if (rd > 1) rd >>= 1;
					break;
				}
			}
		} while (EFI_ERROR(res));

		// Crypt
		if (encrypt) {
			EncryptDataUnits(buf, (UINT64_STRUCT*)&pos, (UINT32)(rd), info);
		}	else {
			DecryptDataUnits(buf, (UINT64_STRUCT*)&pos, (UINT32)(rd), info);
		}

		// Write
		do {
			res = io->WriteBlocks(io, io->Media->MediaId, pos, rd << 9, buf);
			if (EFI_ERROR(res)) {
				UINT8 ari;
				ERR_PRINT(L"Write error: %r\n", res);
				ari = AskARI();
				switch (ari)
				{
				case 'I':
				case 'i':
					res = EFI_SUCCESS;
					break;
				case 'A':
				case 'a':
					goto error;
				case 'R':
				case 'r':
				default:
					break;
				}
			}
		} while (EFI_ERROR(res));

		if (encrypt) {
			pos += rd;
		}	else {
			pos -= rd;
		}
		remains -= rd;

		// Update header
		if (headerInfo != NULL) {
			res = io->ReadBlocks(io, io->Media->MediaId, headerSector, 512, buf);
			if (!EFI_ERROR(res)) {
				UINT32 headerCrc32;
				UINT64 encryptedAreaLength;
				UINT8* headerData;
				if (encrypt) {
					encryptedAreaLength = (size - remains) << 9;
				}	else {
					encryptedAreaLength = remains << 9;
				}
				DecryptBuffer(buf + HEADER_ENCRYPTED_DATA_OFFSET, HEADER_ENCRYPTED_DATA_SIZE, headerInfo);
				if (GetHeaderField32(buf, TC_HEADER_OFFSET_MAGIC) == 0x56455241) {
					headerData = buf + TC_HEADER_OFFSET_ENCRYPTED_AREA_LENGTH;
					mputInt64(headerData, encryptedAreaLength);
					headerCrc32 = GetCrc32(buf + TC_HEADER_OFFSET_MAGIC, TC_HEADER_OFFSET_HEADER_CRC - TC_HEADER_OFFSET_MAGIC);
					headerData = buf + TC_HEADER_OFFSET_HEADER_CRC;
					mputLong(headerData, headerCrc32);
					EncryptBuffer(buf + HEADER_ENCRYPTED_DATA_OFFSET, HEADER_ENCRYPTED_DATA_SIZE, headerInfo);
					res = io->WriteBlocks(io, io->Media->MediaId, headerSector, 512, buf);
				}	else {
					res = EFI_CRC_ERROR;
				}
			}
			if (EFI_ERROR(res)) {
				ERR_PRINT(L"Header update: %r\n", res);
			}
		}

		// Check ESC
		{
			EFI_INPUT_KEY key;
			res = gBS->CheckEvent(gST->ConIn->WaitForKey);
			if(!EFI_ERROR(res)) {
				gST->ConIn->ReadKeyStroke(gST->ConIn, &key);
				if (key.ScanCode == SCAN_ESC) {
					if (AskConfirm("\n\rStop?", 1)) {
						res = EFI_NOT_READY;
						goto error;
					}
				}
			}
		}
	} while (remains > 0);
	RangeCryptProgress(size, remains, pos, remainsOnStart);
	OUT_PRINT(L"\nDone");

error:
	OUT_PRINT(L"\n");
	MEM_FREE(buf);
	return res;
}

EFI_STATUS
VolumeEncrypt(
	IN UINTN index
	)
{
	EFI_STATUS              res;
	EFI_HANDLE              hDisk;
	int                     vcres;
	UINT64                  headerSector;
	EFI_BLOCK_IO_PROTOCOL*  io;

	// Write header
	res = CreateVolumeHeaderOnDisk(index, NULL, &hDisk, &headerSector);
	if (EFI_ERROR(res)) {
		return res;
	}

	// Verify header
	io = EfiGetBlockIO(hDisk);
	if (!io) {
		ERR_PRINT(L"can not get block IO\n");
		return EFI_INVALID_PARAMETER;
	}

	res = io->ReadBlocks(io, io->Media->MediaId, headerSector, 512, Header);
	if (EFI_ERROR(res)) {
		ERR_PRINT(L"Read error %r(%x)\n", res, res);
		return res;
	}

	res = TryHeaderDecrypt(Header, &gAuthCryptInfo, &gHeaderCryptInfo);
	if (EFI_ERROR(res)) {
		return res;
	}

	// Encrypt range
	vcres = AskConfirm("Encrypt?", 1);
	if (!vcres) {
		ERR_PRINT(L"Encryption stoped\n");
		return EFI_INVALID_PARAMETER;
	}

	res = RangeCrypt(hDisk, 
		gAuthCryptInfo->EncryptedAreaStart.Value >> 9, 
		gAuthCryptInfo->VolumeSize.Value >> 9,
		gAuthCryptInfo->EncryptedAreaLength.Value >> 9,
		gAuthCryptInfo, TRUE,
		gHeaderCryptInfo, headerSector);

	crypto_close(gAuthCryptInfo);
	crypto_close(gHeaderCryptInfo);
	return res;
}

EFI_STATUS
VolumeDecrypt(
	IN UINTN index)
{
	EFI_BLOCK_IO_PROTOCOL*  io;
	EFI_STATUS              res;
	EFI_LBA                 vhsector;
	BioPrintDevicePath(index);

	io = EfiGetBlockIO(gBIOHandles[index]);
	if (!io) {
		ERR_PRINT(L"can not get block IO\n");
		return EFI_INVALID_PARAMETER;
	}

	if (gAuthPasswordMsg == NULL) {
		VCAuthAsk();
	}

	vhsector = AskUINT64("header sector:", gAuthBoot? TC_BOOT_VOLUME_HEADER_SECTOR : 0);
	res = io->ReadBlocks(io, io->Media->MediaId, vhsector, 512, Header);
	if (EFI_ERROR(res)) {
		ERR_PRINT(L"Read error %r(%x)\n", res, res);
		return res;
	}

	res = TryHeaderDecrypt(Header, &gAuthCryptInfo, &gHeaderCryptInfo);
	if (EFI_ERROR(res)) {
		return res;
	}

	if (!AskConfirm("Decrypt?", 1)) {
		ERR_PRINT(L"Decryption stoped\n");
		res = EFI_INVALID_PARAMETER;
		goto error;
	}

	res = RangeCrypt(gBIOHandles[index], 
		gAuthCryptInfo->EncryptedAreaStart.Value >> 9, 
		gAuthCryptInfo->VolumeSize.Value >> 9,
		gAuthCryptInfo->EncryptedAreaLength.Value >> 9,
		gAuthCryptInfo, FALSE,
		gHeaderCryptInfo,
		vhsector);

error:
	crypto_close(gHeaderCryptInfo);
	crypto_close(gAuthCryptInfo);
	return res;
}


EFI_STATUS
VolumeChangePassword(
	IN UINTN index
	) 
{
	EFI_BLOCK_IO_PROTOCOL*  io;
	EFI_STATUS              res;
	EFI_LBA                 vhsector;

	BioPrintDevicePath(index);
	io = EfiGetBlockIO(gBIOHandles[index]);
	if (io == NULL) {
		ERR_PRINT(L" No BIO protocol\n");
		return EFI_INVALID_PARAMETER;
	}

	vhsector = gAuthBoot ? TC_BOOT_VOLUME_HEADER_SECTOR : 0;
	vhsector = AskUINT64("sector:", vhsector);
	res = io->ReadBlocks(io, io->Media->MediaId, vhsector, 512, Header);
	if (EFI_ERROR(res)) {
		ERR_PRINT(L"Read error %r(%x)\n", res, res);
		return res;
	}

	res = ChangePassword(Header);
	if (EFI_ERROR(res)) return res;

	if (AskConfirm("Save[N]?", 1)) {
		res = io->WriteBlocks(io, io->Media->MediaId, vhsector, 512, Header);
		ERR_PRINT(L"Header saved: %r\n", res);
	}
	return res;
}

//////////////////////////////////////////////////////////////////////////
// OS Rescue 
//////////////////////////////////////////////////////////////////////////

EFI_STATUS
OSDecrypt()
{

	EFI_STATUS              res;
	UINTN                   disk;
	BOOLEAN                 doDecrypt = FALSE;
	EFI_BLOCK_IO_PROTOCOL*  io;
	if (gAuthPasswordMsg == NULL) {
		VCAuthAsk();
	}

	for (disk = 0; disk < gBIOCount; ++disk) {
		if (EfiIsPartition(gBIOHandles[disk])) continue;
		io = EfiGetBlockIO(gBIOHandles[disk]);
		if (io == NULL) continue;
		res = io->ReadBlocks(io, io->Media->MediaId, 62, 512, Header);
		if (EFI_ERROR(res)) continue;
		BioPrintDevicePath(disk);
		res = TryHeaderDecrypt(Header, &gAuthCryptInfo, &gHeaderCryptInfo);
		if (EFI_ERROR(res)) continue;
		doDecrypt = TRUE;
		break;
	}

	if (doDecrypt) {
		if (!AskConfirm("Decrypt?", 1)) {
			ERR_PRINT(L"Decryption stoped\n");
			return EFI_INVALID_PARAMETER;
		}
		res = RangeCrypt(gBIOHandles[disk], 
			gAuthCryptInfo->EncryptedAreaStart.Value >> 9, 
			gAuthCryptInfo->VolumeSize.Value >> 9,
			gAuthCryptInfo->EncryptedAreaLength.Value >> 9, 
			gAuthCryptInfo, FALSE,
			gHeaderCryptInfo,
			62);
		crypto_close(gHeaderCryptInfo);
		crypto_close(gAuthCryptInfo);
	}
	else {
		res = EFI_NOT_FOUND;
	}
	return res;
}

CHAR16* sOSKeyBackup = L"EFI\\VeraCrypt\\svh_bak";
// dirty import from GptEdit
extern DCS_DISK_ENTRY_DISKID       DeDiskId;

EFI_STATUS
OSBackupKeyLoad(
	UINTN                   *DiskOS
	)
{
	EFI_STATUS              res;
	UINT8                   *restoreData = NULL;
	UINTN                   restoreDataSize;
	UINTN                   disk;
	UINTN                   diskOS;
	EFI_BLOCK_IO_PROTOCOL*  io;
	UINT64                  startUnit = 0;
	INTN                    deListHdrIdOk;

	if (gAuthPasswordMsg == NULL) {
		VCAuthAsk();
	}

	res = FileLoad(NULL, sOSKeyBackup, &SecRegionData, &SecRegionSize);
	if (EFI_ERROR(res) || SecRegionSize < 512) {
		SecRegionSize = 0;
		MEM_FREE(SecRegionData);
	}
	if (SecRegionSize == 0) {
		res = PlatformGetAuthData(&SecRegionData, &SecRegionSize, &SecRegionHandle);
		if (EFI_ERROR(res)) {
			SecRegionSize = 0;
		}
	}

	if (SecRegionSize == 0) {
		return EFI_INVALID_PARAMETER;
	}

	// Try decrypt/locate header (in file or on removable flash)
	do {
		CopyMem(Header, SecRegionData + SecRegionOffset, 512);
		res = TryHeaderDecrypt(Header, &gAuthCryptInfo, NULL);
		if (EFI_ERROR(res)) {
			SecRegionOffset += 128 * 1024;
			if (SecRegionOffset > SecRegionSize) {
				MEM_FREE(SecRegionData);
				SecRegionOffset = 0;
				res = PlatformGetAuthData(&SecRegionData, &SecRegionSize, &SecRegionHandle);
				if (EFI_ERROR(res)) {
					return EFI_INVALID_PARAMETER;
				}
			}
		}
		restoreDataSize = (SecRegionSize - SecRegionOffset >= 128 * 1024)? 128 * 1024 : SecRegionSize - SecRegionOffset;
		restoreData = SecRegionData + SecRegionOffset;
	} while (EFI_ERROR(res));

	// Parse DE list if present
	SetMem(&DeDiskId.GptID, sizeof(DeDiskId.GptID), 0x55);
	SetMem(&DeDiskId.MbrID, sizeof(DeDiskId.MbrID), 0x55);
	if (restoreDataSize >= 1024) {
		deListHdrIdOk = CompareMem(restoreData + 512, &gDcsDiskEntryListHeaderID, sizeof(gDcsDiskEntryListHeaderID));
		if (deListHdrIdOk != 0) {
			DecryptDataUnits(restoreData + 512, (UINT64_STRUCT *)&startUnit, (UINT32)(restoreDataSize >> 9) - 1, gAuthCryptInfo);
			deListHdrIdOk = CompareMem(restoreData + 512, &gDcsDiskEntryListHeaderID, sizeof(gDcsDiskEntryListHeaderID));
			if (deListHdrIdOk != 0) {
				res = EFI_CRC_ERROR;
				goto error;
			}
		}
		res = DeListParseSaved(restoreData);
		if (EFI_ERROR(res)) goto error;
	}

	// Search and list all disks
	diskOS = 999;
	for (disk = 0; disk < gBIOCount; ++disk) {
		if (EfiIsPartition(gBIOHandles[disk])) continue;
		io = EfiGetBlockIO(gBIOHandles[disk]);
		if (io == NULL) continue;
		res = io->ReadBlocks(io, io->Media->MediaId, 0, 512, Header);
		if (EFI_ERROR(res)) continue;
		BioPrintDevicePath(disk);
		if (DeDiskId.MbrID == *(uint32 *)(Header + 0x1b8)) {
			res = io->ReadBlocks(io, io->Media->MediaId, 1, 512, Header);
			if (EFI_ERROR(res)) continue;
			if (CompareMem(&DeDiskId.GptID, &((EFI_PARTITION_TABLE_HEADER*)Header)->DiskGUID, sizeof(DeDiskId.GptID)) == 0) {
				diskOS = disk;
				OUT_PRINT(L"%H[found]%N");
			}
		}
		OUT_PRINT(L"\n");
	}
	diskOS = AskUINTN("Select disk:", diskOS);
	if (diskOS >= gBIOCount) {
		res = EFI_INVALID_PARAMETER;
		goto error;
	}

	if (EfiIsPartition(gBIOHandles[diskOS])) {
		res = EFI_INVALID_PARAMETER;
		goto error;
	}
	*DiskOS = diskOS;
	return EFI_SUCCESS;

error:
	MEM_FREE(SecRegionData);
	return res;
}

EFI_STATUS
OSRestoreKey()
{
	EFI_STATUS              res;
	UINTN                   disk;
	EFI_BLOCK_IO_PROTOCOL*  io;

	res = OSBackupKeyLoad(&disk);
	if (EFI_ERROR(res)) return res;

	if (!AskConfirm("Restore?", 1)) {
		res = EFI_INVALID_PARAMETER;
		goto error;
	}

	io = EfiGetBlockIO(gBIOHandles[disk]);
	if (io == NULL) {
		res = EFI_INVALID_PARAMETER;
		goto error;
	}

	res = io->WriteBlocks(io, io->Media->MediaId, 62, 512, SecRegionData + SecRegionOffset);

error: 
	MEM_FREE(SecRegionData);
	return res;
}

//////////////////////////////////////////////////////////////////////////
// Wipe
//////////////////////////////////////////////////////////////////////////
EFI_STATUS
BlockRangeWipe(
	IN EFI_HANDLE h,
	IN UINT64 start,
	IN UINT64 end
	)
{
	EFI_STATUS              res;
	EFI_BLOCK_IO_PROTOCOL*  bio;
	VOID*                   buf;
	UINT64                  remains;
	UINT64                  pos;
	UINTN                   rd;
	bio = EfiGetBlockIO(h);
	if (bio == 0) {
		ERR_PRINT(L"No block device");
		return EFI_NOT_FOUND;
	}

	res = RndPreapare();
	if (EFI_ERROR(res)) {
		ERR_PRINT(L"Rnd: %r\n", res);
		return res;
	}

	EfiPrintDevicePath(h);

	OUT_PRINT(L"\nSectors [%lld, %lld]", start, end);
	if (AskConfirm(", Wipe data?", 1) == 0) return EFI_NOT_READY;
	buf = MEM_ALLOC(CRYPT_BUF_SECTORS << 9);
	if (!buf) {
		ERR_PRINT(L"can not get buffer\n");
		return EFI_INVALID_PARAMETER;
	}
	remains = end -start + 1;
	pos = start;
	do {
		rd = (UINTN)((remains > CRYPT_BUF_SECTORS) ? CRYPT_BUF_SECTORS : remains);
		RandgetBytes(buf, (UINT32)(rd << 9), FALSE);
		res = bio->WriteBlocks(bio, bio->Media->MediaId, pos, rd << 9, buf);
		if (EFI_ERROR(res)) {
			ERR_PRINT(L"Write error: %r\n", res);
			MEM_FREE(buf);
			return res;
		}
		pos += rd;
		remains -= rd;
		OUT_PRINT(L"%lld %lld       \r", pos, remains);
	} while (remains > 0);
	OUT_PRINT(L"\nDone\n", pos, remains);
	return res;
}

//////////////////////////////////////////////////////////////////////////
// DCS authorization check
//////////////////////////////////////////////////////////////////////////
EFI_STATUS
IntCheckVolume(
	UINTN index
	)
{
	EFI_BLOCK_IO_PROTOCOL*  pBio;
	EFI_STATUS              res;
	EFI_LBA                 vhsector;

	BioPrintDevicePath(index);
	pBio = EfiGetBlockIO(gBIOHandles[index]);
	if (pBio == NULL) {
		ERR_PRINT(L" No BIO protocol\n");
		return EFI_NOT_FOUND;
	}

	vhsector = gAuthBoot ? TC_BOOT_VOLUME_HEADER_SECTOR : 0;
	res = pBio->ReadBlocks(pBio, pBio->Media->MediaId, vhsector, 512, Header);
	if (EFI_ERROR(res)) {
		ERR_PRINT(L" %r(%x)\n", res, res);
		return res;
	}
	
	res = TryHeaderDecrypt(Header, &gAuthCryptInfo, NULL);
	if (res != 0) {
		if (gAuthBoot == 0) {
			OUT_PRINT(L"Try hidden...");
			res = pBio->ReadBlocks(pBio, pBio->Media->MediaId, TC_VOLUME_HEADER_SIZE / 512, 512, Header);
			if (EFI_ERROR(res)) {
				ERR_PRINT(L" %r(%x)\n", res, res);
				return res;
			}
			res = TryHeaderDecrypt(Header, &gAuthCryptInfo, NULL);
		}
	}
	return res;
}

VOID
DisksAuthCheck() {
	UINTN i;
	if (BioIndexStart >= gBIOCount) return;
	i = BioIndexStart;
	do {
		IntCheckVolume(i);
		++i;
	} while ((i < gBIOCount) && (i <= BioIndexEnd));
}

VOID
TestAuthAsk() {
	VCAuthAsk();
}

EFI_STATUS
CreateVolumeHeaderOnDisk(
	IN UINTN      index,
	OUT VOID      **pinfo,
	OUT EFI_HANDLE *phDisk,
	OUT UINT64     *sector
	) 
{
	EFI_BLOCK_IO_PROTOCOL*  bio;
	EFI_STATUS              res;
	UINT64                  encSectorStart = 0;
	UINT64                  encSectorEnd = 0;
	UINT64                  VolumeSize = 0;
	PCRYPTO_INFO            ci = 0;
	EFI_LBA                 vhsector;
	EFI_HANDLE              hDisk = NULL;
	HARDDRIVE_DEVICE_PATH   hdp;
	BOOLEAN                 isPart;

	BioPrintDevicePath(index);
	OUT_PRINT(L"\n");

	isPart = EfiIsPartition(gBIOHandles[index]);
	if (isPart) {
		res = EfiGetPartDetails(gBIOHandles[index], &hdp, &hDisk);
		if (!EFI_ERROR(res)) {
			encSectorEnd = hdp.PartitionSize - encSectorStart - 256;
			VolumeSize = hdp.PartitionSize;
		}
	}

	res = CreateVolumeHeader(Header, &ci, VolumeSize, encSectorStart, encSectorEnd);
	if (EFI_ERROR(res)) {
		return res;
	}

	if (isPart && gAuthBoot) {
		OUT_PRINT(L"Boot drive to save is selected. \n");
		EfiPrintDevicePath(hDisk);
		OUT_PRINT(L"\n");
	}	else {
		hDisk = gBIOHandles[index];
	}

	bio = EfiGetBlockIO(hDisk);
	if (bio == NULL) {
		ERR_PRINT(L"No BIO protocol\n");
		return EFI_NOT_FOUND;
	}

	vhsector = AskUINT64("save to sector:", gAuthBoot ? 62 : 0);
	if (AskConfirm("Save [N]?", 1)) {
		res = bio->WriteBlocks(bio, bio->Media->MediaId, vhsector, 512, Header);
		ERR_PRINT(L"Write: %r\n", res);
	}

	if (phDisk != NULL) *phDisk = hDisk;
	if (pinfo != NULL) {
		*pinfo = ci;
	}	else {
		crypto_close(ci);
	}
	if (sector != NULL)*sector = vhsector;
	return res;
}


//////////////////////////////////////////////////////////////////////////
// USB
//////////////////////////////////////////////////////////////////////////
UINTN       UsbIndex = 0;
void UsbIoPrintDevicePath(UINTN uioIndex) {
	CHAR8*		id = NULL;
	OUT_PRINT(L"%V%d%N ", uioIndex);
	EfiPrintDevicePath(gUSBHandles[uioIndex]);
	UsbGetId(gUSBHandles[uioIndex], &id);
	if (id != NULL) {
		UINT32 rud;
		rud = (UINT32)GetCrc32((unsigned char*)id, (int)AsciiStrLen(id));
		OUT_PRINT(L" -(%d) %a", rud, id);
		MEM_FREE(id);
	}
}

void UsbIoPrintDevicePaths(CHAR16* msg) {
	UINTN i;
	OUT_PRINT(msg);
	for (i = 0; i < gUSBCount; ++i) {
		UsbIoPrintDevicePath(i);
		OUT_PRINT(L"\n");
	}
}

VOID
PrintUsbList() {
	InitUsb();
	UsbIoPrintDevicePaths(L"%HUSB IO handles%N\n");
}


//////////////////////////////////////////////////////////////////////////
// Set DcsInt parameters
//////////////////////////////////////////////////////////////////////////
VOID
UpdateDcsBoot() {
	EFI_STATUS res;
	HARDDRIVE_DEVICE_PATH   dpVolme;
	EFI_HANDLE              hDisk;
	if (BioIndexStart >= gBIOCount) {
		// Delete var
		res = EfiSetVar(DCS_BOOT_STR, &gEfiDcsVariableGuid, NULL, 0, 0);
	}
	else {
		// Set var
		EFI_DEVICE_PATH             *DevicePath;
		UINTN len;
		BioPrintDevicePath(BioIndexStart);
		res = EfiGetPartDetails(gBIOHandles[BioIndexStart], &dpVolme, &hDisk);
		if (EFI_ERROR(res)) {
			OUT_PRINT(L" %r\n", res);
			return;
		}
		DevicePath = DevicePathFromHandle(hDisk);
		len = GetDevicePathSize(DevicePath);
//		res = EfiSetVar(DCS_BOOT_STR, NULL, DevicePath, len, EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS);
		FileSave(NULL, DCS_BOOT_STR, DevicePath, len);
		OUT_PRINT(L"Boot:");
		EfiPrintDevicePath(hDisk);
		OUT_PRINT(L"\n");
	}
	OUT_PRINT(L" %r\n", res);
}

//////////////////////////////////////////////////////////////////////////
// Security region
//////////////////////////////////////////////////////////////////////////
UINTN gSecRigonCount = 0;

EFI_STATUS
SecRigionMark() 
{
	UINT32      crc;
	EFI_STATUS  res;
	DCS_AUTH_DATA_MARK* adm;
	EFI_BLOCK_IO_PROTOCOL* bio;

	res = PlatformGetIDCRC(gBIOHandles[BioIndexStart], &crc);
	if (EFI_ERROR(res)) {
		ERR_PRINT(L"CRC: %r\n", res);
		return res;
	}

	adm = (DCS_AUTH_DATA_MARK*)MEM_ALLOC(512);
	if (adm == NULL) {
		ERR_PRINT(L"no memory\n");
		return EFI_BUFFER_TOO_SMALL;
	}

	adm->AuthDataSize = (UINT32)gSecRigonCount;
	adm->PlatformCrc = crc;
	res = gBS->CalculateCrc32(&adm->PlatformCrc, sizeof(*adm) - 4, &adm->HeaderCrc);

	if (EFI_ERROR(res)) {
		ERR_PRINT(L"CRC: %r\n", res);
		return res;
	}

	bio = EfiGetBlockIO(gBIOHandles[BioIndexStart]);
	if (bio == NULL) {
		MEM_FREE(adm);
		ERR_PRINT(L"No block IO");
		return EFI_ACCESS_DENIED;
	}
	res = bio->WriteBlocks(bio, bio->Media->MediaId, 61, 512, adm);
	if (EFI_ERROR(res)) {
		ERR_PRINT(L"Write: %r\n", res);
	}
	MEM_FREE(adm);
	return res;
}

EFI_STATUS
SecRigionWipe()
{
	EFI_STATUS  res;
	CHAR8*      buf;
	UINTN       i;
	EFI_BLOCK_IO_PROTOCOL* bio;

	buf = MEM_ALLOC(128 * 1024);
	if (buf == NULL) {
		ERR_PRINT(L"no memory\n");
		return EFI_BUFFER_TOO_SMALL;
	}

	bio = EfiGetBlockIO(gBIOHandles[BioIndexStart]);
	if (bio == NULL) {
		ERR_PRINT(L"No block IO");
		res = EFI_ACCESS_DENIED;
		goto error;
	}

	if (!RandgetBytes(buf, 512, FALSE)) {
		ERR_PRINT(L"No randoms\n");
		res = EFI_CRC_ERROR;
		goto error;
	}	
	
	// Wipe mark
	res = bio->WriteBlocks(bio, bio->Media->MediaId, 61, 512, buf);
	if (EFI_ERROR(res)) {
		ERR_PRINT(L"Write: %r\n", res);
		goto error;
	}

	// Wipe region
	for (i = 0; i < gSecRigonCount; ++i) {
		if (!RandgetBytes(buf, 128 * 1024, FALSE)) {
			ERR_PRINT(L"No randoms\n");
			res = EFI_CRC_ERROR;
			goto error;
		}
		res = bio->WriteBlocks(bio, bio->Media->MediaId, 62 + i * (128 * 1024 / 512), 128 * 1024, buf);
		if (EFI_ERROR(res)) {
			ERR_PRINT(L"Write: %r\n", res);
			goto error;
		}
	}
	return EFI_SUCCESS;

error:
	MEM_FREE(buf);
	return res;
}

EFI_STATUS
SecRigionAdd(
	IN UINTN       regIdx
)
{
	EFI_STATUS  res = EFI_SUCCESS;
	EFI_BLOCK_IO_PROTOCOL* bio;
	UINT8*      regionData;
	UINTN       regionSize;
	INTN        deListHdrIdOk;
	res = FileLoad(NULL, (CHAR16*)DcsDiskEntrysFileName, &regionData, &regionSize);
	if (EFI_ERROR(res)) {
		ERR_PRINT(L"Load %s: %r\n", DcsDiskEntrysFileName, res);
		return res;
	}
	deListHdrIdOk = CompareMem(regionData + 512, &gDcsDiskEntryListHeaderID, sizeof(gDcsDiskEntryListHeaderID));

	if (deListHdrIdOk == 0) {
		ERR_PRINT(L"GPT has to be encrypted\n");
		res = EFI_CRC_ERROR;
		goto error;
	}

	bio = EfiGetBlockIO(gBIOHandles[BioIndexStart]);
	if (bio == NULL) {
		ERR_PRINT(L"No block IO");
		res = EFI_ACCESS_DENIED;
		goto error;
	}

	res = bio->WriteBlocks(bio, bio->Media->MediaId, 62 + regIdx * (128 * 1024 / 512), regionSize, regionData);

	if (EFI_ERROR(res)) {
		ERR_PRINT(L"Write: %r\n", res);
		goto error;
	}

error:
	MEM_FREE(regionData);
	return res;
}

//////////////////////////////////////////////////////////////////////////
// GPT
//////////////////////////////////////////////////////////////////////////
EFI_STATUS
GptCryptFile(
	IN BOOLEAN  crypt
	)
{
	EFI_STATUS  res = EFI_SUCCESS;
	UINT64      startUnit = 0;
	UINT8*      regionData;
	UINTN       regionSize;
	INTN        deListHdrIdOk;

	res = FileLoad(NULL, (CHAR16*)DcsDiskEntrysFileName, &regionData, &regionSize);
	if (EFI_ERROR(res)) {
		ERR_PRINT(L"Load %s: %r\n", DcsDiskEntrysFileName, res);
		return res;
	}
	deListHdrIdOk = CompareMem(regionData + 512, &gDcsDiskEntryListHeaderID, sizeof(gDcsDiskEntryListHeaderID));

	if ((deListHdrIdOk != 0) && crypt) {
		ERR_PRINT(L"Already encrypted\n");
		res = EFI_CRC_ERROR;
		goto error;
	}

	if ((deListHdrIdOk == 0) && !crypt) {
		ERR_PRINT(L"Already decrypted\n");
		res = EFI_CRC_ERROR;
		goto error;
	}

	DetectX86Features();
	CopyMem(Header, regionData, sizeof(Header));
	res = TryHeaderDecrypt(Header, &gAuthCryptInfo, NULL);
	if(EFI_ERROR(res)){
		goto error;
	}
	startUnit = 0;
	if (crypt) {
		EncryptDataUnits(regionData + 512, (UINT64_STRUCT *)&startUnit, (UINT32)(regionSize >> 9) - 1, gAuthCryptInfo);
	}
	else {
		DecryptDataUnits(regionData + 512, (UINT64_STRUCT *)&startUnit, (UINT32)(regionSize >> 9) - 1, gAuthCryptInfo);
	}

	res = FileSave(NULL, (CHAR16*)DcsDiskEntrysFileName, regionData, regionSize);
	if (EFI_ERROR(res)) {
		ERR_PRINT(L"Save %s: %r\n", DcsDiskEntrysFileName, res);
		goto error;
	}

error:
	MEM_FREE(regionData);
	return res;
}

EFI_STATUS
GptEdit(
	IN UINTN index
	)
{

	EFI_PARTITION_ENTRY* part = &GptMainEntrys[index];
	
	///
	/// Null-terminated name of the partition.
	///
	CHAR16    PartitionName[36];
	UINTN     len;
	while (!GptAskGUID("type (msr, data, wre, efi, del or guid)\n\r:", &part->PartitionTypeGUID));
	if (CompareMem(&part->PartitionTypeGUID, &gEfiPartTypeUnusedGuid, sizeof(EFI_GUID)) == 0) {
		ZeroMem(part, sizeof(*part));
		GptSqueze();
		GptSort();
		GptSyncMainAlt();
		return EFI_SUCCESS;
	}
	while (!GptAskGUID("id\n\r:", &part->UniquePartitionGUID));
	part->StartingLBA = AskUINT64("StartingLBA:", part->StartingLBA);
	part->EndingLBA = AskUINT64("EndingLBA:", part->EndingLBA);
	part->Attributes = AskHexUINT64("Attributes\n\r:", part->Attributes);
	OUT_PRINT(L"[%s]\n\r:", part->PartitionName);
	GetLine(&len, PartitionName, NULL, sizeof(PartitionName) / 2 - 1, 1);
	if (len != 0) {
		CopyMem(&part->PartitionName, PartitionName, sizeof(PartitionName));
	}
	GptSqueze();
	GptSort();
	GptSyncMainAlt();
	return EFI_SUCCESS;
}
