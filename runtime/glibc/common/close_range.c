/* Close a range of file descriptors in an Android app sandbox.
   Copyright (C) 2021-2025 Free Software Foundation, Inc.
   This file is part of the GNU C Library.

   The GNU C Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   The GNU C Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the GNU C Library; if not, see
   <https://www.gnu.org/licenses/>.  */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <not-cancel.h>
#include <unistd.h>

/* The Android recipe disables the kernel close_range syscall. Retain the
   generic libc fallback, including fd 0, and implement CLOEXEC with fcntl.
   UNSHARE cannot be emulated by modifying the shared descriptor table. */
int
__close_range (unsigned int first, unsigned int last, int flags)
{
  if (first > last || (flags & ~(CLOSE_RANGE_CLOEXEC | CLOSE_RANGE_UNSHARE)))
    {
      __set_errno (EINVAL);
      return -1;
    }
  if (flags & CLOSE_RANGE_UNSHARE)
    {
      __set_errno (ENOSYS);
      return -1;
    }
  /* The common exec preparation case closes everything above a low fd.
     Use upstream's allocation-free procfs walker instead of issuing one
     close syscall per possible descriptor (32768 on Android). It also
     handles EMFILE and descriptors above a subsequently lowered limit.
     Retain the generic path if procfs is unavailable. */
  if (flags == 0 && last >= INT_MAX)
    {
      if (first > INT_MAX)
        return 0;
      int saved_errno = errno;
      if (__closefrom_fallback ((int) first, true))
        {
          __set_errno (saved_errno);
          return 0;
        }
    }
  int maxfd = __getdtablesize ();
  if (maxfd < 0)
    return -1;
  for (unsigned int fd = first; fd <= last && fd < (unsigned int) maxfd; ++fd)
    {
      if (flags & CLOSE_RANGE_CLOEXEC)
        __fcntl64_nocancel (fd, F_SETFD, FD_CLOEXEC);
      else
        __close_nocancel_nostatus (fd);
    }
  return 0;
}
libc_hidden_def (__close_range)
weak_alias (__close_range, close_range)
