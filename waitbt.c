/**
 * Copyright (C) 2011, Shao Miller <shao.miller@yrdsb.edu.on.ca>.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * Wait for the boot disk to become available.
 */

#include <ntddk.h>
#include <stdio.h>
#include <initguid.h>
#include <ntddstor.h>

static VOID WbtPrint(PCHAR message, USHORT size) {
    static WCHAR err_msg[] = L"WaitBt: WbtPrint() failed!\n";
    static UNICODE_STRING uerr_msg = {
        sizeof err_msg,
        sizeof err_msg,
        err_msg,
      };
    NTSTATUS status;
    ANSI_STRING amessage = {
        size,
        size,
        message,
      };
    UNICODE_STRING umessage;

    status = RtlAnsiStringToUnicodeString(&umessage, &amessage, TRUE);
    if (!NT_SUCCESS(status)) {
        ZwDisplayString(&uerr_msg);
        return;
      }
    ZwDisplayString(&umessage);
    RtlFreeUnicodeString(&umessage);
    return;
  }

#define WbtMessage(x_) DbgPrint(x_); WbtPrint(x_, sizeof x_)

static ULONG WbtDiskSignature(HANDLE file) {
    LARGE_INTEGER offset;
    NTSTATUS status;
    IO_STATUS_BLOCK io_status;
    UCHAR mbr[512];
    ULONG sig = 0;

    offset.QuadPart = 0;
    status = ZwReadFile(
        file,
        NULL,
        NULL,
        NULL,
        &io_status,
        mbr,
        sizeof mbr,
        &offset,
        NULL
      );
    if (!NT_SUCCESS(status)) {
        WbtMessage("WaitBt: Couldn't read disk MBR.\n");
        goto out;
      }

    RtlCopyMemory(&sig, mbr + 440, sizeof sig);

    out:
    return sig;
  }

static BOOLEAN WbtCheckBootDisk(ULONG sig, IN PWCHAR dev_name) {
    UNICODE_STRING path;
    OBJECT_ATTRIBUTES obj_attrs;
    NTSTATUS status;
    HANDLE file = 0;
    IO_STATUS_BLOCK io_status;
    BOOLEAN match = FALSE;

    RtlInitUnicodeString(&path, dev_name);
    WbtMessage("WaitBt: Checking disk: ");
    ZwDisplayString(&path);
    WbtMessage(" ...\n");

    InitializeObjectAttributes(
        &obj_attrs,
        &path,
        OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
        NULL,
        NULL
      );
    /* Try this disk. */
    status = ZwCreateFile(
        &file,
        GENERIC_READ,
        &obj_attrs,
        &io_status,
        NULL,
        FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ | FILE_SHARE_DELETE | FILE_SHARE_WRITE,
        FILE_OPEN,
        FILE_NON_DIRECTORY_FILE |
          FILE_RANDOM_ACCESS |
          FILE_SYNCHRONOUS_IO_NONALERT,
        NULL,
        0
      );
    if (!NT_SUCCESS(status)) {
        WbtMessage("WaitBt: Couldn't open disk.\n");
        return match;
      }

    if (WbtDiskSignature(file) == sig)
      /* Found! */
      match = TRUE;

    ZwClose(file);

    return match;
  }

static VOID WbtWaitForBootDisk() {
    static BOOTDISK_INFORMATION info = {0, 0, 0xEFBEADDE, 0xEFBEADDE};
    static CHAR sig_msg[100] = {0};
    NTSTATUS status;
    KEVENT dummy_event;
    LARGE_INTEGER timeout;
    INT sig_msg_size, attempts = 10;
    GUID disk_guid = GUID_DEVINTERFACE_DISK;
    PWSTR sym_links;
    PWCHAR pos;

    KeInitializeEvent(&dummy_event, SynchronizationEvent, FALSE);
    /* 1 second. */
    timeout.QuadPart = -10000000LL;

    while (attempts--) {
        /* Fetch the boot disk information. */
        status = IoGetBootDiskInformation(&info, sizeof info);
        if (!NT_SUCCESS(status)) {
            WbtMessage("WaitBt: Couldn't read boot disk MBR signature!\n");
            goto wait;
          }
        if (
            info.BootDeviceSignature == 0xEFBEADDE ||
            info.SystemDeviceSignature == 0xEFBEADDE
          ) {
            WbtMessage("WaitBt: Disk signature(s) not provided!\n");
            goto wait;
          }

        sig_msg_size = sprintf(
            sig_msg,
            "WaitBt: Boot sig: 0x%08X Sys sig: 0x%08X\n",
            info.BootDeviceSignature,
            info.SystemDeviceSignature
          );
        if (sig_msg_size < 1) {
            WbtMessage("WaitBt: Cannot display disk signatures!\n");
            goto wait;
          }
        DbgPrint(sig_msg);
        WbtPrint(sig_msg, (USHORT)sig_msg_size);

        /* Fetch the list of disks. */
        status = IoGetDeviceInterfaces(&disk_guid, NULL, 0, &sym_links);
        if (!NT_SUCCESS(status)) {
            WbtMessage("WaitBt: Couldn't enumerate disks!\n");
            goto wait;
          }

        pos = sym_links;
        /* Process each disk, looking for the boot disk. */
        while (*pos != UNICODE_NULL) {
            if (WbtCheckBootDisk(info.BootDeviceSignature, pos)) {
                /* Found it. */
                WbtMessage("WaitBt: Found!\n");
                ExFreePool(sym_links);
                return;
              }
            /* Step to the next disk. */
            while (*pos != UNICODE_NULL)
              pos++;
            pos++;
          }

        ExFreePool(sym_links);
        /* Wait for 1 second, then try again. */
        wait:
        WbtMessage("WaitBt: Waiting 1 second...\n");
        KeWaitForSingleObject(
            &dummy_event,
            Executive,
            KernelMode,
            FALSE,
            &timeout
          );
        KeResetEvent(&dummy_event);
      }
    WbtMessage("WaitBt: Failed 10 times!\n");
    /* Give the user 10 seconds to read this status. */
    attempts = 10;
    while (attempts--) {
        KeWaitForSingleObject(
            &dummy_event,
            Executive,
            KernelMode,
            FALSE,
            &timeout
          );
        KeResetEvent(&dummy_event);
      }
    return;
  }

NTSTATUS DriverEntry(
    IN PDRIVER_OBJECT DriverObject,
    IN PUNICODE_STRING RegistryPath
  ) {
    WbtMessage("WaitBt: Alive\n");
    WbtWaitForBootDisk();
    return STATUS_SUCCESS;
  }


