------------------------------------------------------------------------------
--                                                                          --
--                GNU ADA RUN-TIME LIBRARY (GNARL) COMPONENTS               --
--                                                                          --
--                 S Y S T E M . T A S K _ P R I M I T I V E S              --
--                                                                          --
--                                  S p e c                                 --
--                                                                          --
--                             $Revision: 1.1.1.1 $
--                                                                          --
--          Copyright (C) 1992-2000, Free Software Foundation, Inc.         --
--                                                                          --
-- GNARL is free software; you can  redistribute it  and/or modify it under --
-- terms of the  GNU General Public License as published  by the Free Soft- --
-- ware  Foundation;  either version 2,  or (at your option) any later ver- --
-- sion. GNARL is distributed in the hope that it will be useful, but WITH- --
-- OUT ANY WARRANTY;  without even the  implied warranty of MERCHANTABILITY --
-- or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License --
-- for  more details.  You should have  received  a copy of the GNU General --
-- Public License  distributed with GNARL; see file COPYING.  If not, write --
-- to  the Free Software Foundation,  59 Temple Place - Suite 330,  Boston, --
-- MA 02111-1307, USA.                                                      --
--                                                                          --
-- As a special exception,  if other files  instantiate  generics from this --
-- unit, or you link  this unit with other files  to produce an executable, --
-- this  unit  does not  by itself cause  the resulting  executable  to  be --
-- covered  by the  GNU  General  Public  License.  This exception does not --
-- however invalidate  any other reasons why  the executable file  might be --
-- covered by the  GNU Public License.                                      --
--                                                                          --
-- GNARL was developed by the GNARL team at Florida State University.       --
-- Extensive contributions were provided by Ada Core Technologies Inc.      --
--                                                                          --
------------------------------------------------------------------------------

--  This is a Solaris version of this package.
--  It was created by hand for use with new "checked"
--  GNULLI primitives.

--  This package provides low-level support for most tasking features.

pragma Polling (Off);
--  Turn off polling, we do not want ATC polling to take place during
--  tasking operations. It causes infinite loops and other problems.

with System.OS_Interface;
--  used for mutex_t
--           cond_t
--           thread_t

package System.Task_Primitives is
   pragma Preelaborate;

   type Lock is limited private;
   type Lock_Ptr is access all Lock;
   --  Should be used for implementation of protected objects.

   type RTS_Lock is limited private;
   type RTS_Lock_Ptr is access all RTS_Lock;
   --  Should be used inside the runtime system.
   --  The difference between Lock and the RTS_Lock is that the later
   --  one serves only as a semaphore so that do not check for
   --  ceiling violations.

   type Task_Body_Access is access procedure;
   --  Pointer to the task body's entry point (or possibly a wrapper
   --  declared local to the GNARL).

   type Private_Data is limited private;
   --  Any information that the GNULLI needs maintained on a per-task
   --  basis.  A component of this type is guaranteed to be included
   --  in the Ada_Task_Control_Block.

private

   type Private_Task_Serial_Number is mod 2 ** 64;
   --  Used to give each task a unique serial number.

   type Base_Lock is new System.OS_Interface.mutex_t;

   type Owner_Int is new Integer;
   for Owner_Int'Alignment use Standard'Maximum_Alignment;

   type Owner_ID is access all Owner_Int;

   type Lock is record
      L : aliased Base_Lock;
      Ceiling : System.Any_Priority := System.Any_Priority'First;
      Saved_Priority : System.Any_Priority :=  System.Any_Priority'First;
      Owner : Owner_ID;
      Next  : Lock_Ptr;
      Level : Private_Task_Serial_Number := 0;
      Buddy : Owner_ID;
      Frozen : Boolean := False;
   end record;

   type RTS_Lock is new Lock;

   --  Note that task support on gdb relies on the fact that the first
   --  2 fields of Private_Data are Thread and LWP.

   type Private_Data is record
      Thread      : aliased System.OS_Interface.thread_t;
      pragma Atomic (Thread);
      --  Thread field may be updated by two different threads of control.
      --  (See, Enter_Task and Create_Task in s-taprop.adb).
      --  They put the same value (thr_self value). We do not want to
      --  use lock on those operations and the only thing we have to
      --  make sure is that they are updated in atomic fashion.

      LWP : System.OS_Interface.lwpid_t;
      --  The LWP id of the thread. Set by self in Enter_Task.

      CV          : aliased System.OS_Interface.cond_t;
      L           : aliased RTS_Lock;
      --  protection for all components is lock L

      Active_Priority : System.Any_Priority := System.Any_Priority'First;
      --  Simulated active priority,
      --  used only if Priority_Ceiling_Support is True.

      Locking : Lock_Ptr;
      Locks : Lock_Ptr;
      Wakeups : Natural := 0;
   end record;

end System.Task_Primitives;
