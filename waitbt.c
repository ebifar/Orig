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

/** Macros */
#define WbtMessage(x_) DbgPrint(x_); WbtPrint(x_, sizeof x_)
#define M_WBT_READ_TIME (-100000000LL)
#define M_WBT_DELAY_TIME (-10000000LL)

/* Handy for arrays. */
#define COUNTOF(array_) \
  (sizeof (array_) / sizeof *(array_))
#define FOR_EACH_ELEMENT(index_, array_) \
  for ((index_) = 0; (index_) < COUNTOF(array_); (index_)++)

/** Constants */
enum {
    CvWbtDummySig = 0xEFBEADDE,
    CvWbtMaxAttempts = 10,
    CvWbtMsgBufSize = 100,
    CvWbtZero = 0
  };

/** Object types */
typedef union {
    LONGLONG longlong;
    LARGE_INTEGER large_int;
  } U_WBT_LARGE_INT;
typedef struct {
    const GUID * from_guid;
    PCHAR intf_name;
    PVOID registration;
    GUID guid;
  } S_WBT_INTF;
typedef CHAR A_WBT_MSG[CvWbtMsgBufSize];

/** Function declarations */
extern int snprintf(char *, size_t, const char *, ...);

/** Object definitions */
static S_WBT_INTF WbtIntfs[] = {
    {&GUID_DEVINTERFACE_DISK, "Disk", 0},
    {&GUID_DEVINTERFACE_PARTITION, "Partition", 0},
    {&GUID_DEVINTERFACE_STORAGEPORT, "Storage Port", 0},
    {&GUID_DEVINTERFACE_VOLUME, "Volume", 0},
  };

/** Function definitions */

/* Diplay a debugging message */
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

/* Display the failure status to the user */
static VOID WbtFailure(void) {
    static U_WBT_LARGE_INT read_time = {M_WBT_READ_TIME};

    WbtMessage("WaitBt: Maximum failed attempts reached!\n");
    /* Give the user some time to read this status. */
    KeDelayExecutionThread(KernelMode, FALSE, &read_time.large_int);
    return;
  }

/* De-register for notification of certain device interface arrivals */
static VOID WbtDeregisterIntfNotifications(void) {
    SIZE_T i;
    NTSTATUS status;

    FOR_EACH_ELEMENT(i, WbtIntfs) {
        status = IoUnregisterPlugPlayNotification(WbtIntfs[i].registration);
        if (!NT_SUCCESS(status)) {
            INT msg_size;
            A_WBT_MSG msg;

            msg_size = sprintf(
                msg,
                "WaitBt: Still getting %s notifications!\n",
                WbtIntfs[i].intf_name
              );
            if (msg_size < 1) {
                WbtMessage("WaitBt: Message problem!\n");
              } else {
                DbgPrint(msg);
                WbtPrint(msg, (USHORT)msg_size);
              }
          }
      }
    return;
  }

/* Attempt to find the disk with the boot volume */
static NTSTATUS WbtFindBootDisk(
    IN PDRIVER_OBJECT drv_obj,
    IN PVOID context,
    IN ULONG attempts
  ) {
    static BOOTDISK_INFORMATION info = {0, 0, CvWbtDummySig, CvWbtDummySig};
    static A_WBT_MSG sig_msg = {0};
    static U_WBT_LARGE_INT delay_time = {M_WBT_DELAY_TIME};
    NTSTATUS status;
    INT sig_msg_size;

    /* Fetch the boot disk information. */
    status = IoGetBootDiskInformation(&info, sizeof info);
    if (!NT_SUCCESS(status)) {
        WbtMessage("WaitBt: Couldn't read boot disk information!\n");
        goto retry;
      }
    /* Check that it's been properly filled */
    if (
        info.BootDeviceSignature == CvWbtDummySig ||
        info.SystemDeviceSignature == CvWbtDummySig
      ) {
        WbtMessage("WaitBt: Disk signature(s) not provided!\n");
        goto retry;
      }
    /* Display the signature information to the user */
    sig_msg_size = sprintf(
        sig_msg,
        "WaitBt: Boot sig: 0x%08X Sys sig: 0x%08X\n",
        info.BootDeviceSignature,
        info.SystemDeviceSignature
      );
    if (sig_msg_size < 1) {
        WbtMessage("WaitBt: Cannot display disk signatures!\n");
        goto retry;
      }
    DbgPrint(sig_msg);
    WbtPrint(sig_msg, (USHORT)sig_msg_size);
    WbtDeregisterIntfNotifications();
    return STATUS_SUCCESS;

    retry:
    /* Check if we've reached the maximum number of allowed attempts */
    if (attempts == CvWbtMaxAttempts) {
        WbtFailure();
        return STATUS_UNSUCCESSFUL;
      }
    /* Delay and re-schedule a boot disk search */
    WbtMessage("WaitBt: Waiting...\n");
    KeDelayExecutionThread(KernelMode, FALSE, &delay_time.large_int);
    IoRegisterBootDriverReinitialization(drv_obj, WbtFindBootDisk, NULL);
    return STATUS_SUCCESS;
  }

static NTSTATUS WbtInterfaceArrived(
    IN PVOID notification,
    IN PVOID context
  ) {
    INT msg_size;
    A_WBT_MSG msg;
    ANSI_STRING msg_str = {sizeof msg, sizeof msg, msg};
    UNICODE_STRING msg_ustr = {sizeof msg, sizeof msg, 0};
    PDEVICE_INTERFACE_CHANGE_NOTIFICATION notice;
    NTSTATUS status;

    notice = notification;
    msg_size = sprintf(msg, "WaitBt: %s arrived: ", context);
    if (msg_size < 1) {
        WbtMessage("WaitBt: Message problem!\n");
        return STATUS_UNSUCCESSFUL;
      } else {
        DbgPrint(msg);
        WbtPrint(msg, (USHORT)msg_size);
      }
    msg_ustr.Buffer = notice->SymbolicLinkName->Buffer;
    msg_ustr.Length = notice->SymbolicLinkName->Length < sizeof msg ?
      notice->SymbolicLinkName->Length :
      sizeof msg;

    status = RtlUnicodeStringToAnsiString(
        &msg_str,
        &msg_ustr,
        FALSE
      );
    if (!NT_SUCCESS(status)) {
        WbtMessage("WaitBt: Message problem!\n");
        return STATUS_UNSUCCESSFUL;
      } else {
        DbgPrint(msg);
        WbtPrint(msg, notice->SymbolicLinkName->Length);
      }
    WbtMessage("\n");

    return STATUS_SUCCESS;
  }

/* Register for notification of certain device interface arrivals */
static VOID WbtRegisterIntfNotifications(PDRIVER_OBJECT drv_obj) {
    SIZE_T i;
    NTSTATUS status;

    FOR_EACH_ELEMENT(i, WbtIntfs) {
        WbtIntfs[i].guid = *WbtIntfs[i].from_guid;
        status = IoRegisterPlugPlayNotification(
            EventCategoryDeviceInterfaceChange,
            PNPNOTIFY_DEVICE_INTERFACE_INCLUDE_EXISTING_INTERFACES,
            &WbtIntfs[i].guid,
            drv_obj,
            WbtInterfaceArrived,
            WbtIntfs[i].intf_name,
            &WbtIntfs[i].registration
          );
        if (!NT_SUCCESS(status)) {
            INT msg_size;
            A_WBT_MSG msg;

            msg_size = sprintf(
                msg,
                "WaitBt: %s notifications failed!\n",
                WbtIntfs[i].intf_name
              );
            if (msg_size < 1) {
                WbtMessage("WaitBt: Message problem!\n");
              } else {
                DbgPrint(msg);
                WbtPrint(msg, (USHORT)msg_size);
              }
          }
      }
    return;
  }

/* The driver entry-point */
NTSTATUS DriverEntry(
    IN PDRIVER_OBJECT DriverObj,
    IN PUNICODE_STRING RegPath
  ) {
    WbtMessage("WaitBt: Alive\n");

    WbtRegisterIntfNotifications(DriverObj);

    /* Schedule a boot disk search */
    IoRegisterBootDriverReinitialization(DriverObj, WbtFindBootDisk, NULL);
    return STATUS_SUCCESS;
  }
