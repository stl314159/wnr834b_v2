------------------------------------------------------------------------------
--                                                                          --
--                GNU ADA RUN-TIME LIBRARY (GNARL) COMPONENTS               --
--                                                                          --
--     S Y S T E M . T A S K _ P R I M I T I V E S . O P E R A T I O N S    --
--                                                                          --
--                                  B o d y                                 --
--                                                                          --
--                             $Revision: 1.1.1.1 $
--                                                                          --
--             Copyright (C) 1991-2001, Florida State University            --
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
-- GNARL was developed by the GNARL team at Florida State University. It is --
-- now maintained by Ada Core Technologies Inc. in cooperation with Florida --
-- State University (http://www.gnat.com).                                  --
--                                                                          --
------------------------------------------------------------------------------

--  This is a POSIX-like version of this package

--  This package contains all the GNULL primitives that interface directly
--  with the underlying OS.

--  Note: this file can only be used for POSIX compliant systems that
--  implement SCHED_FIFO and Ceiling Locking correctly.

--  For configurations where SCHED_FIFO and priority ceiling are not a
--  requirement, this file can also be used (e.g AiX threads)

pragma Polling (Off);
--  Turn off polling, we do not want ATC polling to take place during
--  tasking operations. It causes infinite loops and other problems.

with System.Tasking.Debug;
--  used for Known_Tasks

with System.Task_Info;
--  used for Task_Info_Type

with Interfaces.C;
--  used for int
--           size_t

with System.Interrupt_Management;
--  used for Keep_Unmasked
--           Abort_Task_Interrupt
--           Interrupt_ID

with System.Interrupt_Management.Operations;
--  used for Set_Interrupt_Mask
--           All_Tasks_Mask
pragma Elaborate_All (System.Interrupt_Management.Operations);

with System.Parameters;
--  used for Size_Type

with System.Tasking;
--  used for Ada_Task_Control_Block
--           Task_ID

with System.Soft_Links;
--  used for Defer/Undefer_Abort

--  Note that we do not use System.Tasking.Initialization directly since
--  this is a higher level package that we shouldn't depend on. For example
--  when using the restricted run time, it is replaced by
--  System.Tasking.Restricted.Initialization

with System.OS_Primitives;
--  used for Delay_Modes

with Unchecked_Conversion;
with Unchecked_Deallocation;

package body System.Task_Primitives.Operations is

   use System.Tasking.Debug;
   use System.Tasking;
   use Interfaces.C;
   use System.OS_Interface;
   use System.Parameters;
   use System.OS_Primitives;

   package SSL renames System.Soft_Links;

   ------------------
   --  Local Data  --
   ------------------

   --  The followings are logically constants, but need to be initialized
   --  at run time.

   All_Tasks_L : aliased System.Task_Primitives.RTS_Lock;
   --  See comments on locking rules in System.Tasking (spec).

   Environment_Task_ID : Task_ID;
   --  A variable to hold Task_ID for the environment task.

   Locking_Policy : Character;
   pragma Import (C, Locking_Policy, "__gl_locking_policy");
   --  Value of the pragma Locking_Policy:
   --    'C' for Ceiling_Locking
   --    'I' for Inherit_Locking
   --    ' ' for none.

   Unblocked_Signal_Mask : aliased sigset_t;
   --  The set of signals that should unblocked in all tasks

   --  The followings are internal configuration constants needed.

   Next_Serial_Number : Task_Serial_Number := 100;
   --  We start at 100, to reserve some special values for
   --  using in error checking.

   Time_Slice_Val : Integer;
   pragma Import (C, Time_Slice_Val, "__gl_time_slice_val");

   Dispatching_Policy : Character;
   pragma Import (C, Dispatching_Policy, "__gl_task_dispatching_policy");

   FIFO_Within_Priorities : constant Boolean := Dispatching_Policy = 'F';
   --  Indicates whether FIFO_Within_Priorities is set.

   -----------------------
   -- Local Subprograms --
   -----------------------

   procedure Abort_Handler
     (Sig     : Signal);

   function To_Task_ID is new Unchecked_Conversion (System.Address, Task_ID);

   function To_Address is new Unchecked_Conversion (Task_ID, System.Address);

   --------------------
   -- Local Packages --
   --------------------

   package Specific is

      procedure Initialize (Environment_Task : Task_ID);
      pragma Inline (Initialize);
      --  Initialize various data needed by this package.

      procedure Set (Self_Id : Task_ID);
      pragma Inline (Set);
      --  Set the self id for the current task.

      function Self return Task_ID;
      pragma Inline (Self);
      --  Return a pointer to the Ada Task Control Block of the calling task.

   end Specific;

   package body Specific is separate;
   --  The body of this package is target specific.

   -------------------
   -- Abort_Handler --
   -------------------

   --  Target-dependent binding of inter-thread Abort signal to
   --  the raising of the Abort_Signal exception.

   --  The technical issues and alternatives here are essentially
   --  the same as for raising exceptions in response to other
   --  signals (e.g. Storage_Error). See code and comments in
   --  the package body System.Interrupt_Management.

   --  Some implementations may not allow an exception to be propagated
   --  out of a handler, and others might leave the signal or
   --  interrupt that invoked this handler masked after the exceptional
   --  return to the application code.

   --  GNAT exceptions are originally implemented using setjmp()/longjmp().
   --  On most UNIX systems, this will allow transfer out of a signal handler,
   --  which is usually the only mechanism available for implementing
   --  asynchronous handlers of this kind. However, some
   --  systems do not restore the signal mask on longjmp(), leaving the
   --  abort signal masked.

   --  Alternative solutions include:

   --       1. Change the PC saved in the system-dependent Context
   --          parameter to point to code that raises the exception.
   --          Normal return from this handler will then raise
   --          the exception after the mask and other system state has
   --          been restored (see example below).

   --       2. Use siglongjmp()/sigsetjmp() to implement exceptions.

   --       3. Unmask the signal in the Abortion_Signal exception handler
   --          (in the RTS).

   --  The following procedure would be needed if we can't lonjmp out of
   --  a signal handler  (See below)

   --  procedure Raise_Abort_Signal is
   --  begin
   --     raise Standard'Abort_Signal;
   --  end if;

   procedure Abort_Handler
     (Sig     : Signal) is

      T       : Task_ID := Self;
      Result  : Interfaces.C.int;
      Old_Set : aliased sigset_t;

   begin
      --  Assuming it is safe to longjmp out of a signal handler, the
      --  following code can be used:

      if T.Deferral_Level = 0
        and then T.Pending_ATC_Level < T.ATC_Nesting_Level and then
        not T.Aborting
      then
         T.Aborting := True;

         --  Make sure signals used for RTS internal purpose are unmasked

         Result := pthread_sigmask (SIG_UNBLOCK,
           Unblocked_Signal_Mask'Unchecked_Access, Old_Set'Unchecked_Access);
         pragma Assert (Result = 0);

         raise Standard'Abort_Signal;
      end if;

      --  Otherwise, something like this is required:
      --  if not Abort_Is_Deferred.all then
      --    --  Overwrite the return PC address with the address of the
      --    --  special raise routine, and "return" to that routine's
      --    --  starting address.
      --    Context.PC := Raise_Abort_Signal'Address;
      --    return;
      --  end if;

   end Abort_Handler;

   -------------------
   --  Stack_Guard  --
   -------------------

   procedure Stack_Guard (T : ST.Task_ID; On : Boolean) is

      Stack_Base : constant Address := Get_Stack_Base (T.Common.LL.Thread);
      Guard_Page_Address : Address;

      Res : Interfaces.C.int;

   begin
      if Stack_Base_Available then
         --  Compute the guard page address

         Guard_Page_Address :=
           Stack_Base - (Stack_Base mod Get_Page_Size) + Get_Page_Size;

         if On then
            Res := mprotect (Guard_Page_Address, Get_Page_Size, PROT_ON);
         else
            Res := mprotect (Guard_Page_Address, Get_Page_Size, PROT_OFF);
         end if;

         pragma Assert (Res = 0);
      end if;
   end Stack_Guard;

   --------------------
   -- Get_Thread_Id  --
   --------------------

   function Get_Thread_Id (T : ST.Task_ID) return OSI.Thread_Id is
   begin
      return T.Common.LL.Thread;
   end Get_Thread_Id;

   ----------
   -- Self --
   ----------

   function Self return Task_ID renames Specific.Self;

   ---------------------
   -- Initialize_Lock --
   ---------------------

   --  Note: mutexes and cond_variables needed per-task basis are
   --        initialized in Initialize_TCB and the Storage_Error is
   --        handled. Other mutexes (such as All_Tasks_Lock, Memory_Lock...)
   --        used in RTS is initialized before any status change of RTS.
   --        Therefore rasing Storage_Error in the following routines
   --        should be able to be handled safely.

   procedure Initialize_Lock
     (Prio : System.Any_Priority;
      L    : access Lock)
   is
      Attributes : aliased pthread_mutexattr_t;
      Result : Interfaces.C.int;

   begin
      Result := pthread_mutexattr_init (Attributes'Access);
      pragma Assert (Result = 0 or else Result = ENOMEM);

      if Result = ENOMEM then
         raise Storage_Error;
      end if;

      if Locking_Policy = 'C' then
         Result := pthread_mutexattr_setprotocol
           (Attributes'Access, PTHREAD_PRIO_PROTECT);
         pragma Assert (Result = 0);

         Result := pthread_mutexattr_setprioceiling
            (Attributes'Access, Interfaces.C.int (Prio));
         pragma Assert (Result = 0);

      elsif Locking_Policy = 'I' then
         Result := pthread_mutexattr_setprotocol
           (Attributes'Access, PTHREAD_PRIO_INHERIT);
         pragma Assert (Result = 0);
      end if;

      Result := pthread_mutex_init (L, Attributes'Access);
      pragma Assert (Result = 0 or else Result = ENOMEM);

      if Result = ENOMEM then
         raise Storage_Error;
      end if;

      Result := pthread_mutexattr_destroy (Attributes'Access);
      pragma Assert (Result = 0);
   end Initialize_Lock;

   procedure Initialize_Lock (L : access RTS_Lock; Level : Lock_Level) is
      Attributes : aliased pthread_mutexattr_t;
      Result : Interfaces.C.int;

   begin
      Result := pthread_mutexattr_init (Attributes'Access);
      pragma Assert (Result = 0 or else Result = ENOMEM);

      if Result = ENOMEM then
         raise Storage_Error;
      end if;

      if Locking_Policy = 'C' then
         Result := pthread_mutexattr_setprotocol
           (Attributes'Access, PTHREAD_PRIO_PROTECT);
         pragma Assert (Result = 0);

         Result := pthread_mutexattr_setprioceiling
            (Attributes'Access, Interfaces.C.int (System.Any_Priority'Last));
         pragma Assert (Result = 0);

      elsif Locking_Policy = 'I' then
         Result := pthread_mutexattr_setprotocol
           (Attributes'Access, PTHREAD_PRIO_INHERIT);
         pragma Assert (Result = 0);
      end if;

      Result := pthread_mutex_init (L, Attributes'Access);
      pragma Assert (Result = 0 or else Result = ENOMEM);

      if Result = ENOMEM then
         Result := pthread_mutexattr_destroy (Attributes'Access);
         raise Storage_Error;
      end if;

      Result := pthread_mutexattr_destroy (Attributes'Access);
      pragma Assert (Result = 0);
   end Initialize_Lock;

   -------------------
   -- Finalize_Lock --
   -------------------

   procedure Finalize_Lock (L : access Lock) is
      Result : Interfaces.C.int;

   begin
      Result := pthread_mutex_destroy (L);
      pragma Assert (Result = 0);
   end Finalize_Lock;

   procedure Finalize_Lock (L : access RTS_Lock) is
      Result : Interfaces.C.int;

   begin
      Result := pthread_mutex_destroy (L);
      pragma Assert (Result = 0);
   end Finalize_Lock;

   ----------------
   -- Write_Lock --
   ----------------

   procedure Write_Lock (L : access Lock; Ceiling_Violation : out Boolean) is
      Result : Interfaces.C.int;

   begin
      Result := pthread_mutex_lock (L);

      --  Assume that the cause of EINVAL is a priority ceiling violation

      Ceiling_Violation := (Result = EINVAL);
      pragma Assert (Result = 0 or else Result = EINVAL);
   end Write_Lock;

   procedure Write_Lock (L : access RTS_Lock) is
      Result : Interfaces.C.int;

   begin
      Result := pthread_mutex_lock (L);
      pragma Assert (Result = 0);
   end Write_Lock;

   procedure Write_Lock (T : Task_ID) is
      Result : Interfaces.C.int;

   begin
      Result := pthread_mutex_lock (T.Common.LL.L'Access);
      pragma Assert (Result = 0);
   end Write_Lock;

   ---------------
   -- Read_Lock --
   ---------------

   procedure Read_Lock (L : access Lock; Ceiling_Violation : out Boolean) is
   begin
      Write_Lock (L, Ceiling_Violation);
   end Read_Lock;

   ------------
   -- Unlock --
   ------------

   procedure Unlock (L : access Lock) is
      Result : Interfaces.C.int;

   begin
      Result := pthread_mutex_unlock (L);
      pragma Assert (Result = 0);
   end Unlock;

   procedure Unlock (L : access RTS_Lock) is
      Result : Interfaces.C.int;

   begin
      Result := pthread_mutex_unlock (L);
      pragma Assert (Result = 0);
   end Unlock;

   procedure Unlock (T : Task_ID) is
      Result : Interfaces.C.int;

   begin
      Result := pthread_mutex_unlock (T.Common.LL.L'Access);
      pragma Assert (Result = 0);
   end Unlock;

   -------------
   --  Sleep  --
   -------------

   procedure Sleep (Self_ID : Task_ID;
                    Reason   : System.Tasking.Task_States) is
      Result : Interfaces.C.int;

   begin
      pragma Assert (Self_ID = Self);
      Result := pthread_cond_wait (Self_ID.Common.LL.CV'Access,
        Self_ID.Common.LL.L'Access);

      --  EINTR is not considered a failure.

      pragma Assert (Result = 0 or else Result = EINTR);
   end Sleep;

   -----------------
   -- Timed_Sleep --
   -----------------

   --  This is for use within the run-time system, so abort is
   --  assumed to be already deferred, and the caller should be
   --  holding its own ATCB lock.

   procedure Timed_Sleep
     (Self_ID  : Task_ID;
      Time     : Duration;
      Mode     : ST.Delay_Modes;
      Reason   : Task_States;
      Timedout : out Boolean;
      Yielded  : out Boolean)
   is
      Check_Time : constant Duration := Monotonic_Clock;
      Rel_Time   : Duration;
      Abs_Time   : Duration;
      Request    : aliased timespec;
      Result     : Interfaces.C.int;

   begin
      Timedout := True;
      Yielded := False;

      if Mode = Relative then
         Abs_Time := Duration'Min (Time, Max_Sensible_Delay) + Check_Time;

         if Relative_Timed_Wait then
            Rel_Time := Duration'Min (Max_Sensible_Delay, Time);
         end if;

      else
         Abs_Time := Duration'Min (Check_Time + Max_Sensible_Delay, Time);

         if Relative_Timed_Wait then
            Rel_Time := Duration'Min (Max_Sensible_Delay, Time - Check_Time);
         end if;
      end if;

      if Abs_Time > Check_Time then
         if Relative_Timed_Wait then
            Request := To_Timespec (Rel_Time);
         else
            Request := To_Timespec (Abs_Time);
         end if;

         loop
            exit when Self_ID.Pending_ATC_Level < Self_ID.ATC_Nesting_Level
              or else Self_ID.Pending_Priority_Change;

            Result := pthread_cond_timedwait (Self_ID.Common.LL.CV'Access,
              Self_ID.Common.LL.L'Access, Request'Access);

            exit when Abs_Time <= Monotonic_Clock;

            if Result = 0 or Result = EINTR then

               --  Somebody may have called Wakeup for us

               Timedout := False;
               exit;
            end if;

            pragma Assert (Result = ETIMEDOUT);
         end loop;
      end if;
   end Timed_Sleep;

   -----------------
   -- Timed_Delay --
   -----------------

   --  This is for use in implementing delay statements, so
   --  we assume the caller is abort-deferred but is holding
   --  no locks.

   procedure Timed_Delay
     (Self_ID  : Task_ID;
      Time     : Duration;
      Mode     : ST.Delay_Modes)
   is
      Check_Time : constant Duration := Monotonic_Clock;
      Abs_Time   : Duration;
      Rel_Time   : Duration;
      Request    : aliased timespec;
      Result     : Interfaces.C.int;

   begin
      --  Only the little window between deferring abort and
      --  locking Self_ID is the reason we need to
      --  check for pending abort and priority change below! :(

      SSL.Abort_Defer.all;
      Write_Lock (Self_ID);

      if Mode = Relative then
         Abs_Time := Duration'Min (Time, Max_Sensible_Delay) + Check_Time;

         if Relative_Timed_Wait then
            Rel_Time := Duration'Min (Max_Sensible_Delay, Time);
         end if;

      else
         Abs_Time := Duration'Min (Check_Time + Max_Sensible_Delay, Time);

         if Relative_Timed_Wait then
            Rel_Time := Duration'Min (Max_Sensible_Delay, Time - Check_Time);
         end if;
      end if;

      if Abs_Time > Check_Time then
         if Relative_Timed_Wait then
            Request := To_Timespec (Rel_Time);
         else
            Request := To_Timespec (Abs_Time);
         end if;

         Self_ID.Common.State := Delay_Sleep;

         loop
            if Self_ID.Pending_Priority_Change then
               Self_ID.Pending_Priority_Change := False;
               Self_ID.Common.Base_Priority := Self_ID.New_Base_Priority;
               Set_Priority (Self_ID, Self_ID.Common.Base_Priority);
            end if;

            exit when Self_ID.Pending_ATC_Level < Self_ID.ATC_Nesting_Level;

            Result := pthread_cond_timedwait (Self_ID.Common.LL.CV'Access,
              Self_ID.Common.LL.L'Access, Request'Access);
            exit when Abs_Time <= Monotonic_Clock;

            pragma Assert (Result = 0
                             or else Result = ETIMEDOUT
                             or else Result = EINTR);
         end loop;

         Self_ID.Common.State := Runnable;
      end if;

      Unlock (Self_ID);
      Result := sched_yield;
      SSL.Abort_Undefer.all;
   end Timed_Delay;

   ---------------------
   -- Monotonic_Clock --
   ---------------------

   function Monotonic_Clock return Duration is
      TS     : aliased timespec;
      Result : Interfaces.C.int;

   begin
      Result := clock_gettime
        (clock_id => CLOCK_REALTIME, tp => TS'Unchecked_Access);
      pragma Assert (Result = 0);
      return To_Duration (TS);
   end Monotonic_Clock;

   -------------------
   -- RT_Resolution --
   -------------------

   function RT_Resolution return Duration is
   begin
      return 10#1.0#E-6;
   end RT_Resolution;

   ------------
   -- Wakeup --
   ------------

   procedure Wakeup (T : Task_ID; Reason : System.Tasking.Task_States) is
      Result : Interfaces.C.int;

   begin
      Result := pthread_cond_signal (T.Common.LL.CV'Access);
      pragma Assert (Result = 0);
   end Wakeup;

   -----------
   -- Yield --
   -----------

   procedure Yield (Do_Yield : Boolean := True) is
      Result : Interfaces.C.int;

   begin
      if Do_Yield then
         Result := sched_yield;
      end if;
   end Yield;

   ------------------
   -- Set_Priority --
   ------------------

   procedure Set_Priority
     (T : Task_ID;
      Prio : System.Any_Priority;
      Loss_Of_Inheritance : Boolean := False)
   is
      Result : Interfaces.C.int;
      Param  : aliased struct_sched_param;

   begin
      T.Common.Current_Priority := Prio;
      Param.sched_priority := Interfaces.C.int (Prio);

      if Time_Slice_Supported and then Time_Slice_Val > 0 then
         Result := pthread_setschedparam
           (T.Common.LL.Thread, SCHED_RR, Param'Access);

      elsif FIFO_Within_Priorities or else Time_Slice_Val = 0 then
         Result := pthread_setschedparam
           (T.Common.LL.Thread, SCHED_FIFO, Param'Access);

      else
         Result := pthread_setschedparam
           (T.Common.LL.Thread, SCHED_OTHER, Param'Access);
      end if;

      pragma Assert (Result = 0);
   end Set_Priority;

   ------------------
   -- Get_Priority --
   ------------------

   function Get_Priority (T : Task_ID) return System.Any_Priority is
   begin
      return T.Common.Current_Priority;
   end Get_Priority;

   ----------------
   -- Enter_Task --
   ----------------

   procedure Enter_Task (Self_ID : Task_ID) is
   begin
      Self_ID.Common.LL.Thread := pthread_self;
      Self_ID.Common.LL.LWP := lwp_self;

      Specific.Set (Self_ID);

      Lock_All_Tasks_List;

      for I in Known_Tasks'Range loop
         if Known_Tasks (I) = null then
            Known_Tasks (I) := Self_ID;
            Self_ID.Known_Tasks_Index := I;
            exit;
         end if;
      end loop;

      Unlock_All_Tasks_List;
   end Enter_Task;

   --------------
   -- New_ATCB --
   --------------

   function New_ATCB (Entry_Num : Task_Entry_Index) return Task_ID is
   begin
      return new Ada_Task_Control_Block (Entry_Num);
   end New_ATCB;

   ----------------------
   --  Initialize_TCB  --
   ----------------------

   procedure Initialize_TCB (Self_ID : Task_ID; Succeeded : out Boolean) is
      Mutex_Attr : aliased pthread_mutexattr_t;
      Result : Interfaces.C.int;
      Cond_Attr : aliased pthread_condattr_t;

   begin
      --  Give the task a unique serial number.

      Self_ID.Serial_Number := Next_Serial_Number;
      Next_Serial_Number := Next_Serial_Number + 1;
      pragma Assert (Next_Serial_Number /= 0);

      Result := pthread_mutexattr_init (Mutex_Attr'Access);
      pragma Assert (Result = 0 or else Result = ENOMEM);

      if Result /= 0 then
         Succeeded := False;
         return;
      end if;

      Result := pthread_mutexattr_setprotocol
        (Mutex_Attr'Access, PTHREAD_PRIO_PROTECT);
      pragma Assert (Result = 0);

      Result := pthread_mutexattr_setprioceiling
        (Mutex_Attr'Access, Interfaces.C.int (System.Any_Priority'Last));
      pragma Assert (Result = 0);

      Result := pthread_mutex_init (Self_ID.Common.LL.L'Access,
        Mutex_Attr'Access);
      pragma Assert (Result = 0 or else Result = ENOMEM);

      if Result /= 0 then
         Succeeded := False;
         return;
      end if;

      Result := pthread_mutexattr_destroy (Mutex_Attr'Access);
      pragma Assert (Result = 0);

      Result := pthread_condattr_init (Cond_Attr'Access);
      pragma Assert (Result = 0 or else Result = ENOMEM);

      if Result /= 0 then
         Result := pthread_mutex_destroy (Self_ID.Common.LL.L'Access);
         pragma Assert (Result = 0);
         Succeeded := False;
         return;
      end if;

      Result := pthread_cond_init (Self_ID.Common.LL.CV'Access,
        Cond_Attr'Access);
      pragma Assert (Result = 0 or else Result = ENOMEM);

      if Result = 0 then
         Succeeded := True;
      else
         Result := pthread_mutex_destroy (Self_ID.Common.LL.L'Access);
         pragma Assert (Result = 0);
         Succeeded := False;
      end if;

      Result := pthread_condattr_destroy (Cond_Attr'Access);
      pragma Assert (Result = 0);
   end Initialize_TCB;

   -----------------
   -- Create_Task --
   -----------------

   procedure Create_Task
     (T          : Task_ID;
      Wrapper    : System.Address;
      Stack_Size : System.Parameters.Size_Type;
      Priority   : System.Any_Priority;
      Succeeded  : out Boolean)
   is
      Attributes          : aliased pthread_attr_t;
      Adjusted_Stack_Size : Interfaces.C.size_t;
      Result              : Interfaces.C.int;

      function Thread_Body_Access is new
        Unchecked_Conversion (System.Address, Thread_Body);

      use System.Task_Info;

   begin
      if Stack_Size = Unspecified_Size then
         Adjusted_Stack_Size := Interfaces.C.size_t (Default_Stack_Size);

      elsif Stack_Size < Minimum_Stack_Size then
         Adjusted_Stack_Size := Interfaces.C.size_t (Minimum_Stack_Size);

      else
         Adjusted_Stack_Size := Interfaces.C.size_t (Stack_Size);
      end if;

      if Stack_Base_Available then
         --  If Stack Checking is supported then allocate 2 additional pages:
         --
         --  In the worst case, stack is allocated at something like
         --  N * Get_Page_Size - epsilon, we need to add the size for 2 pages
         --  to be sure the effective stack size is greater than what
         --  has been asked.

         Adjusted_Stack_Size := Adjusted_Stack_Size + 2 * Get_Page_Size;
      end if;

      Result := pthread_attr_init (Attributes'Access);
      pragma Assert (Result = 0 or else Result = ENOMEM);

      if Result /= 0 then
         Succeeded := False;
         return;
      end if;

      Result := pthread_attr_setdetachstate
        (Attributes'Access, PTHREAD_CREATE_DETACHED);
      pragma Assert (Result = 0);

      Result := pthread_attr_setstacksize
        (Attributes'Access, Adjusted_Stack_Size);
      pragma Assert (Result = 0);

      if T.Common.Task_Info /= Default_Scope then

         --  We are assuming that Scope_Type has the same values than the
         --  corresponding C macros

         Result := pthread_attr_setscope
           (Attributes'Access, Task_Info_Type'Pos (T.Common.Task_Info));
         pragma Assert (Result = 0);
      end if;

      --  Since the initial signal mask of a thread is inherited from the
      --  creator, and the Environment task has all its signals masked, we
      --  do not need to manipulate caller's signal mask at this point.
      --  All tasks in RTS will have All_Tasks_Mask initially.

      Result := pthread_create
        (T.Common.LL.Thread'Access,
         Attributes'Access,
         Thread_Body_Access (Wrapper),
         To_Address (T));
      pragma Assert (Result = 0 or else Result = EAGAIN);

      Succeeded := Result = 0;

      Result := pthread_attr_destroy (Attributes'Access);
      pragma Assert (Result = 0);

      Set_Priority (T, Priority);
   end Create_Task;

   ------------------
   -- Finalize_TCB --
   ------------------

   procedure Finalize_TCB (T : Task_ID) is
      Result : Interfaces.C.int;
      Tmp    : Task_ID := T;

      procedure Free is new
        Unchecked_Deallocation (Ada_Task_Control_Block, Task_ID);

   begin
      Result := pthread_mutex_destroy (T.Common.LL.L'Access);
      pragma Assert (Result = 0);

      Result := pthread_cond_destroy (T.Common.LL.CV'Access);
      pragma Assert (Result = 0);

      if T.Known_Tasks_Index /= -1 then
         Known_Tasks (T.Known_Tasks_Index) := null;
      end if;

      Free (Tmp);
   end Finalize_TCB;

   ---------------
   -- Exit_Task --
   ---------------

   procedure Exit_Task is
   begin
      pthread_exit (System.Null_Address);
   end Exit_Task;

   ----------------
   -- Abort_Task --
   ----------------

   procedure Abort_Task (T : Task_ID) is
      Result : Interfaces.C.int;

   begin
      Result := pthread_kill (T.Common.LL.Thread,
        Signal (System.Interrupt_Management.Abort_Task_Interrupt));
      pragma Assert (Result = 0);
   end Abort_Task;

   ----------------
   -- Check_Exit --
   ----------------

   --  Dummy versions. The only currently working versions is for solaris
   --  (native).

   function Check_Exit (Self_ID : ST.Task_ID) return Boolean is
   begin
      return True;
   end Check_Exit;

   --------------------
   -- Check_No_Locks --
   --------------------

   function Check_No_Locks (Self_ID : ST.Task_ID) return Boolean is
   begin
      return True;
   end Check_No_Locks;

   ----------------------
   -- Environment_Task --
   ----------------------

   function Environment_Task return Task_ID is
   begin
      return Environment_Task_ID;
   end Environment_Task;

   -------------------------
   -- Lock_All_Tasks_List --
   -------------------------

   procedure Lock_All_Tasks_List is
   begin
      Write_Lock (All_Tasks_L'Access);
   end Lock_All_Tasks_List;

   ---------------------------
   -- Unlock_All_Tasks_List --
   ---------------------------

   procedure Unlock_All_Tasks_List is
   begin
      Unlock (All_Tasks_L'Access);
   end Unlock_All_Tasks_List;

   ------------------
   -- Suspend_Task --
   ------------------

   function Suspend_Task
     (T           : ST.Task_ID;
      Thread_Self : Thread_Id) return Boolean is
   begin
      return False;
   end Suspend_Task;

   -----------------
   -- Resume_Task --
   -----------------

   function Resume_Task
     (T           : ST.Task_ID;
      Thread_Self : Thread_Id) return Boolean is
   begin
      return False;
   end Resume_Task;

   ----------------
   -- Initialize --
   ----------------

   procedure Initialize (Environment_Task : Task_ID) is
      act     : aliased struct_sigaction;
      old_act : aliased struct_sigaction;
      Tmp_Set : aliased sigset_t;
      Result  : Interfaces.C.int;

   begin
      Environment_Task_ID := Environment_Task;

      --  Initialize the lock used to synchronize chain of all ATCBs.

      Initialize_Lock (All_Tasks_L'Access, All_Tasks_Level);

      Specific.Initialize (Environment_Task);

      Enter_Task (Environment_Task);

      --  Install the abort-signal handler

      act.sa_flags := 0;
      act.sa_handler := Abort_Handler'Address;

      Result := sigemptyset (Tmp_Set'Access);
      pragma Assert (Result = 0);
      act.sa_mask := Tmp_Set;

      Result :=
        sigaction (
          Signal (System.Interrupt_Management.Abort_Task_Interrupt),
          act'Unchecked_Access,
          old_act'Unchecked_Access);

      pragma Assert (Result = 0);
   end Initialize;

begin
   declare
      Result : Interfaces.C.int;

   begin
      --  Mask Environment task for all signals. The original mask of the
      --  Environment task will be recovered by Interrupt_Server task
      --  during the elaboration of s-interr.adb.

      System.Interrupt_Management.Operations.Set_Interrupt_Mask
        (System.Interrupt_Management.Operations.All_Tasks_Mask'Access);

      --  Prepare the set of signals that should unblocked in all tasks

      Result := sigemptyset (Unblocked_Signal_Mask'Access);
      pragma Assert (Result = 0);

      for J in Interrupt_Management.Interrupt_ID loop
         if System.Interrupt_Management.Keep_Unmasked (J) then
            Result := sigaddset (Unblocked_Signal_Mask'Access, Signal (J));
            pragma Assert (Result = 0);
         end if;
      end loop;
   end;

end System.Task_Primitives.Operations;
