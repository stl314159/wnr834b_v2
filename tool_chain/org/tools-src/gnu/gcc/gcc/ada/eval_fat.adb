------------------------------------------------------------------------------
--                                                                          --
--                         GNAT COMPILER COMPONENTS                         --
--                                                                          --
--                             E V A L _ F A T                              --
--                                                                          --
--                                 B o d y                                  --
--                                                                          --
--                            $Revision: 1.1.1.1 $
--                                                                          --
--          Copyright (C) 1992-2001 Free Software Foundation, Inc.          --
--                                                                          --
-- GNAT is free software;  you can  redistribute it  and/or modify it under --
-- terms of the  GNU General Public License as published  by the Free Soft- --
-- ware  Foundation;  either version 2,  or (at your option) any later ver- --
-- sion.  GNAT is distributed in the hope that it will be useful, but WITH- --
-- OUT ANY WARRANTY;  without even the  implied warranty of MERCHANTABILITY --
-- or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License --
-- for  more details.  You should have  received  a copy of the GNU General --
-- Public License  distributed with GNAT;  see file COPYING.  If not, write --
-- to  the Free Software Foundation,  59 Temple Place - Suite 330,  Boston, --
-- MA 02111-1307, USA.                                                      --
--                                                                          --
-- GNAT was originally developed  by the GNAT team at  New York University. --
-- Extensive contributions were provided by Ada Core Technologies Inc.      --
--                                                                          --
------------------------------------------------------------------------------

with Einfo;    use Einfo;
with Sem_Util; use Sem_Util;
with Ttypef;   use Ttypef;
with Targparm; use Targparm;

package body Eval_Fat is

   Radix : constant Int := 2;
   --  This code is currently only correct for the radix 2 case. We use
   --  the symbolic value Radix where possible to help in the unlikely
   --  case of anyone ever having to adjust this code for another value,
   --  and for documentation purposes.

   type Radix_Power_Table is array (Int range 1 .. 4) of Int;

   Radix_Powers : constant Radix_Power_Table
     := (Radix**1, Radix**2, Radix**3, Radix**4);

   function Float_Radix return T renames Ureal_2;
   --  Radix expressed in real form

   -----------------------
   -- Local Subprograms --
   -----------------------

   procedure Decompose
     (RT       : R;
      X        : in T;
      Fraction : out T;
      Exponent : out UI;
      Mode     : Rounding_Mode := Round);
   --  Decomposes a non-zero floating-point number into fraction and
   --  exponent parts. The fraction is in the interval 1.0 / Radix ..
   --  T'Pred (1.0) and uses Rbase = Radix.
   --  The result is rounded to a nearest machine number.

   procedure Decompose_Int
     (RT               : R;
      X                : in T;
      Fraction         : out UI;
      Exponent         : out UI;
      Mode             : Rounding_Mode);
   --  This is similar to Decompose, except that the Fraction value returned
   --  is an integer representing the value Fraction * Scale, where Scale is
   --  the value (Radix ** Machine_Mantissa (RT)). The value is obtained by
   --  using biased rounding (halfway cases round away from zero), round to
   --  even, a floor operation or a ceiling operation depending on the setting
   --  of Mode (see corresponding descriptions in Urealp).
   --  In case rounding was specified, Rounding_Was_Biased is set True
   --  if the input was indeed halfway between to machine numbers and
   --  got rounded away from zero to an odd number.

   function Eps_Model (RT : R) return T;
   --  Return the smallest model number of R.

   function Eps_Denorm (RT : R) return T;
   --  Return the smallest denormal of type R.

   function Machine_Mantissa (RT : R) return Nat;
   --  Get value of machine mantissa

   --------------
   -- Adjacent --
   --------------

   function Adjacent (RT : R; X, Towards : T) return T is
   begin
      if Towards = X then
         return X;

      elsif Towards > X then
         return Succ (RT, X);

      else
         return Pred (RT, X);
      end if;
   end Adjacent;

   -------------
   -- Ceiling --
   -------------

   function Ceiling (RT : R; X : T) return T is
      XT : constant T := Truncation (RT, X);

   begin
      if UR_Is_Negative (X) then
         return XT;

      elsif X = XT then
         return X;

      else
         return XT + Ureal_1;
      end if;
   end Ceiling;

   -------------
   -- Compose --
   -------------

   function Compose (RT : R; Fraction : T; Exponent : UI) return T is
      Arg_Frac : T;
      Arg_Exp  : UI;

   begin
      if UR_Is_Zero (Fraction) then
         return Fraction;
      else
         Decompose (RT, Fraction, Arg_Frac, Arg_Exp);
         return Scaling (RT, Arg_Frac, Exponent);
      end if;
   end Compose;

   ---------------
   -- Copy_Sign --
   ---------------

   function Copy_Sign (RT : R; Value, Sign : T) return T is
      Result : T;

   begin
      Result := abs Value;

      if UR_Is_Negative (Sign) then
         return -Result;
      else
         return Result;
      end if;
   end Copy_Sign;

   ---------------
   -- Decompose --
   ---------------

   procedure Decompose
     (RT       : R;
      X        : in T;
      Fraction : out T;
      Exponent : out UI;
      Mode     : Rounding_Mode := Round)
   is
      Int_F : UI;

   begin
      Decompose_Int (RT, abs X, Int_F, Exponent, Mode);

      Fraction := UR_From_Components
       (Num      => Int_F,
        Den      => UI_From_Int (Machine_Mantissa (RT)),
        Rbase    => Radix,
        Negative => False);

      if UR_Is_Negative (X) then
         Fraction := -Fraction;
      end if;

      return;
   end Decompose;

   -------------------
   -- Decompose_Int --
   -------------------

   --  This procedure should be modified with care, as there
   --  are many non-obvious details that may cause problems
   --  that are hard to detect. The cases of positive and
   --  negative zeroes are also special and should be
   --  verified separately.

   procedure Decompose_Int
     (RT               : R;
      X                : in T;
      Fraction         : out UI;
      Exponent         : out UI;
      Mode             : Rounding_Mode)
   is
      Base : Int := Rbase (X);
      N    : UI  := abs Numerator (X);
      D    : UI  := Denominator (X);

      N_Times_Radix : UI;

      Even : Boolean;
      --  True iff Fraction is even

      Most_Significant_Digit : constant UI :=
                                 Radix ** (Machine_Mantissa (RT) - 1);

      Uintp_Mark : Uintp.Save_Mark;
      --  The code is divided into blocks that systematically release
      --  intermediate values (this routine generates lots of junk!)

   begin
      Calculate_D_And_Exponent_1 : begin
         Uintp_Mark := Mark;
         Exponent := Uint_0;

         --  In cases where Base > 1, the actual denominator is
         --  Base**D. For cases where Base is a power of Radix, use
         --  the value 1 for the Denominator and adjust the exponent.

         --  Note: Exponent has different sign from D, because D is a divisor

         for Power in 1 .. Radix_Powers'Last loop
            if Base = Radix_Powers (Power) then
               Exponent := -D * Power;
               Base := 0;
               D := Uint_1;
               exit;
            end if;
         end loop;

         Release_And_Save (Uintp_Mark, D, Exponent);
      end Calculate_D_And_Exponent_1;

      if Base > 0 then
         Calculate_Exponent : begin
            Uintp_Mark := Mark;

            --  For bases that are a multiple of the Radix, divide
            --  the base by Radix and adjust the Exponent. This will
            --  help because D will be much smaller and faster to process.

            --  This occurs for decimal bases on a machine with binary
            --  floating-point for example. When calculating 1E40,
            --  with Radix = 2, N will be 93 bits instead of 133.

            --        N            E
            --      ------  * Radix
            --           D
            --       Base

            --                  N                        E
            --    =  --------------------------  *  Radix
            --                     D        D
            --         (Base/Radix)  * Radix

            --             N                  E-D
            --    =  ---------------  *  Radix
            --                    D
            --        (Base/Radix)

            --  This code is commented out, because it causes numerous
            --  failures in the regression suite. To be studied ???

            while False and then Base > 0 and then Base mod Radix = 0 loop
               Base := Base / Radix;
               Exponent := Exponent + D;
            end loop;

            Release_And_Save (Uintp_Mark, Exponent);
         end Calculate_Exponent;

         --  For remaining bases we must actually compute
         --  the exponentiation.

         --  Because the exponentiation can be negative, and D must
         --  be integer, the numerator is corrected instead.

         Calculate_N_And_D : begin
            Uintp_Mark := Mark;

            if D < 0 then
               N := N * Base ** (-D);
               D := Uint_1;
            else
               D := Base ** D;
            end if;

            Release_And_Save (Uintp_Mark, N, D);
         end Calculate_N_And_D;

         Base := 0;
      end if;

      --  Now scale N and D so that N / D is a value in the
      --  interval [1.0 / Radix, 1.0) and adjust Exponent accordingly,
      --  so the value N / D * Radix ** Exponent remains unchanged.

      --  Step 1 - Adjust N so N / D >= 1 / Radix, or N = 0

      --  N and D are positive, so N / D >= 1 / Radix implies N * Radix >= D.
      --  This scaling is not possible for N is Uint_0 as there
      --  is no way to scale Uint_0 so the first digit is non-zero.

      Calculate_N_And_Exponent : begin
         Uintp_Mark := Mark;

         N_Times_Radix := N * Radix;

         if N /= Uint_0 then
            while not (N_Times_Radix >= D) loop
               N := N_Times_Radix;
               Exponent := Exponent - 1;

               N_Times_Radix := N * Radix;
            end loop;
         end if;

         Release_And_Save (Uintp_Mark, N, Exponent);
      end Calculate_N_And_Exponent;

      --  Step 2 - Adjust D so N / D < 1

      --  Scale up D so N / D < 1, so N < D

      Calculate_D_And_Exponent_2 : begin
         Uintp_Mark := Mark;

         while not (N < D) loop

            --  As N / D >= 1, N / (D * Radix) will be at least 1 / Radix,
            --  so the result of Step 1 stays valid

            D := D * Radix;
            Exponent := Exponent + 1;
         end loop;

         Release_And_Save (Uintp_Mark, D, Exponent);
      end Calculate_D_And_Exponent_2;

      --  Here the value N / D is in the range [1.0 / Radix .. 1.0)

      --  Now find the fraction by doing a very simple-minded
      --  division until enough digits have been computed.

      --  This division works for all radices, but is only efficient for
      --  a binary radix. It is just like a manual division algorithm,
      --  but instead of moving the denominator one digit right, we move
      --  the numerator one digit left so the numerator and denominator
      --  remain integral.

      Fraction := Uint_0;
      Even := True;

      Calculate_Fraction_And_N : begin
         Uintp_Mark := Mark;

         loop
            while N >= D loop
               N := N - D;
               Fraction := Fraction + 1;
               Even := not Even;
            end loop;

            --  Stop when the result is in [1.0 / Radix, 1.0)

            exit when Fraction >= Most_Significant_Digit;

            N := N * Radix;
            Fraction := Fraction * Radix;
            Even := True;
         end loop;

         Release_And_Save (Uintp_Mark, Fraction, N);
      end Calculate_Fraction_And_N;

      Calculate_Fraction_And_Exponent : begin
         Uintp_Mark := Mark;

         --  Put back sign before applying the rounding.

         if UR_Is_Negative (X) then
            Fraction := -Fraction;
         end if;

         --  Determine correct rounding based on the remainder
         --  which is in N and the divisor D.

         Rounding_Was_Biased := False; -- Until proven otherwise

         case Mode is
            when Round_Even =>

               --  This rounding mode should not be used for static
               --  expressions, but only for compile-time evaluation
               --  of non-static expressions.

               if (Even and then N * 2 > D)
                     or else
                  (not Even and then N * 2 >= D)
               then
                  Fraction := Fraction + 1;
               end if;

            when Round   =>

               --  Do not round to even as is done with IEEE arithmetic,
               --  but instead round away from zero when the result is
               --  exactly between two machine numbers. See RM 4.9(38).

               if N * 2 >= D then
                  Fraction := Fraction + 1;

                  Rounding_Was_Biased := Even and then N * 2 = D;
                  --  Check for the case where the result is actually
                  --  different from Round_Even.
               end if;

            when Ceiling =>
               if N > Uint_0 then
                  Fraction := Fraction + 1;
               end if;

            when Floor   => null;
         end case;

         --  The result must be normalized to [1.0/Radix, 1.0),
         --  so adjust if the result is 1.0 because of rounding.

         if Fraction = Most_Significant_Digit * Radix then
            Fraction := Most_Significant_Digit;
            Exponent := Exponent + 1;
         end if;

         Release_And_Save (Uintp_Mark, Fraction, Exponent);
      end Calculate_Fraction_And_Exponent;

   end Decompose_Int;

   ----------------
   -- Eps_Denorm --
   ----------------

   function Eps_Denorm (RT : R) return T is
      Digs : constant UI := Digits_Value (RT);
      Emin : Int;
      Mant : Int;

   begin
      if Vax_Float (RT) then
         if Digs = VAXFF_Digits then
            Emin := VAXFF_Machine_Emin;
            Mant := VAXFF_Machine_Mantissa;

         elsif Digs = VAXDF_Digits then
            Emin := VAXDF_Machine_Emin;
            Mant := VAXDF_Machine_Mantissa;

         else
            pragma Assert (Digs = VAXGF_Digits);
            Emin := VAXGF_Machine_Emin;
            Mant := VAXGF_Machine_Mantissa;
         end if;

      elsif Is_AAMP_Float (RT) then
         if Digs = AAMPS_Digits then
            Emin := AAMPS_Machine_Emin;
            Mant := AAMPS_Machine_Mantissa;

         else
            pragma Assert (Digs = AAMPL_Digits);
            Emin := AAMPL_Machine_Emin;
            Mant := AAMPL_Machine_Mantissa;
         end if;

      else
         if Digs = IEEES_Digits then
            Emin := IEEES_Machine_Emin;
            Mant := IEEES_Machine_Mantissa;

         elsif Digs = IEEEL_Digits then
            Emin := IEEEL_Machine_Emin;
            Mant := IEEEL_Machine_Mantissa;

         else
            pragma Assert (Digs = IEEEX_Digits);
            Emin := IEEEX_Machine_Emin;
            Mant := IEEEX_Machine_Mantissa;
         end if;
      end if;

      return Float_Radix ** UI_From_Int (Emin - Mant);
   end Eps_Denorm;

   ---------------
   -- Eps_Model --
   ---------------

   function Eps_Model (RT : R) return T is
      Digs : constant UI := Digits_Value (RT);
      Emin : Int;

   begin
      if Vax_Float (RT) then
         if Digs = VAXFF_Digits then
            Emin := VAXFF_Machine_Emin;

         elsif Digs = VAXDF_Digits then
            Emin := VAXDF_Machine_Emin;

         else
            pragma Assert (Digs = VAXGF_Digits);
            Emin := VAXGF_Machine_Emin;
         end if;

      elsif Is_AAMP_Float (RT) then
         if Digs = AAMPS_Digits then
            Emin := AAMPS_Machine_Emin;

         else
            pragma Assert (Digs = AAMPL_Digits);
            Emin := AAMPL_Machine_Emin;
         end if;

      else
         if Digs = IEEES_Digits then
            Emin := IEEES_Machine_Emin;

         elsif Digs = IEEEL_Digits then
            Emin := IEEEL_Machine_Emin;

         else
            pragma Assert (Digs = IEEEX_Digits);
            Emin := IEEEX_Machine_Emin;
         end if;
      end if;

      return Float_Radix ** UI_From_Int (Emin);
   end Eps_Model;

   --------------
   -- Exponent --
   --------------

   function Exponent (RT : R; X : T) return UI is
      X_Frac : UI;
      X_Exp  : UI;

   begin
      if UR_Is_Zero (X) then
         return Uint_0;
      else
         Decompose_Int (RT, X, X_Frac, X_Exp, Round_Even);
         return X_Exp;
      end if;
   end Exponent;

   -----------
   -- Floor --
   -----------

   function Floor (RT : R; X : T) return T is
      XT : constant T := Truncation (RT, X);

   begin
      if UR_Is_Positive (X) then
         return XT;

      elsif XT = X then
         return X;

      else
         return XT - Ureal_1;
      end if;
   end Floor;

   --------------
   -- Fraction --
   --------------

   function Fraction (RT : R; X : T) return T is
      X_Frac : T;
      X_Exp  : UI;

   begin
      if UR_Is_Zero (X) then
         return X;
      else
         Decompose (RT, X, X_Frac, X_Exp);
         return X_Frac;
      end if;
   end Fraction;

   ------------------
   -- Leading_Part --
   ------------------

   function Leading_Part (RT : R; X : T; Radix_Digits : UI) return T is
      L    : UI;
      Y, Z : T;

   begin
      if Radix_Digits >= Machine_Mantissa (RT) then
         return X;

      else
         L := Exponent (RT, X) - Radix_Digits;
         Y := Truncation (RT, Scaling (RT, X, -L));
         Z := Scaling (RT, Y, L);
         return Z;
      end if;

   end Leading_Part;

   -------------
   -- Machine --
   -------------

   function Machine (RT : R; X : T; Mode : Rounding_Mode) return T is
      X_Frac : T;
      X_Exp  : UI;

   begin
      if UR_Is_Zero (X) then
         return X;
      else
         Decompose (RT, X, X_Frac, X_Exp, Mode);
         return Scaling (RT, X_Frac, X_Exp);
      end if;
   end Machine;

   ----------------------
   -- Machine_Mantissa --
   ----------------------

   function Machine_Mantissa (RT : R) return Nat is
      Digs : constant UI := Digits_Value (RT);
      Mant : Nat;

   begin
      if Vax_Float (RT) then
         if Digs = VAXFF_Digits then
            Mant := VAXFF_Machine_Mantissa;

         elsif Digs = VAXDF_Digits then
            Mant := VAXDF_Machine_Mantissa;

         else
            pragma Assert (Digs = VAXGF_Digits);
            Mant := VAXGF_Machine_Mantissa;
         end if;

      elsif Is_AAMP_Float (RT) then
         if Digs = AAMPS_Digits then
            Mant := AAMPS_Machine_Mantissa;

         else
            pragma Assert (Digs = AAMPL_Digits);
            Mant := AAMPL_Machine_Mantissa;
         end if;

      else
         if Digs = IEEES_Digits then
            Mant := IEEES_Machine_Mantissa;

         elsif Digs = IEEEL_Digits then
            Mant := IEEEL_Machine_Mantissa;

         else
            pragma Assert (Digs = IEEEX_Digits);
            Mant := IEEEX_Machine_Mantissa;
         end if;
      end if;

      return Mant;
   end Machine_Mantissa;

   -----------
   -- Model --
   -----------

   function Model (RT : R; X : T) return T is
      X_Frac : T;
      X_Exp  : UI;

   begin
      Decompose (RT, X, X_Frac, X_Exp);
      return Compose (RT, X_Frac, X_Exp);
   end Model;

   ----------
   -- Pred --
   ----------

   function Pred (RT : R; X : T) return T is
      Result_F : UI;
      Result_X : UI;

   begin
      if abs X < Eps_Model (RT) then
         if Denorm_On_Target then
            return X - Eps_Denorm (RT);

         elsif X > Ureal_0 then
            --  Target does not support denorms, so predecessor is 0.0
            return Ureal_0;

         else
            --  Target does not support denorms, and X is 0.0
            --  or at least bigger than -Eps_Model (RT)

            return -Eps_Model (RT);
         end if;

      else
         Decompose_Int (RT, X, Result_F,  Result_X, Ceiling);
         return UR_From_Components
           (Num      => Result_F - 1,
            Den      => Machine_Mantissa (RT) - Result_X,
            Rbase    => Radix,
            Negative => False);
         --  Result_F may be false, but this is OK as UR_From_Components
         --  handles that situation.
      end if;
   end Pred;

   ---------------
   -- Remainder --
   ---------------

   function Remainder (RT : R; X, Y : T) return T is
      A        : T;
      B        : T;
      Arg      : T;
      P        : T;
      Arg_Frac : T;
      P_Frac   : T;
      Sign_X   : T;
      IEEE_Rem : T;
      Arg_Exp  : UI;
      P_Exp    : UI;
      K        : UI;
      P_Even   : Boolean;

   begin
      if UR_Is_Positive (X) then
         Sign_X :=  Ureal_1;
      else
         Sign_X := -Ureal_1;
      end if;

      Arg := abs X;
      P   := abs Y;

      if Arg < P then
         P_Even := True;
         IEEE_Rem := Arg;
         P_Exp := Exponent (RT, P);

      else
         --  ??? what about zero cases?
         Decompose (RT, Arg, Arg_Frac, Arg_Exp);
         Decompose (RT, P,   P_Frac,   P_Exp);

         P := Compose (RT, P_Frac, Arg_Exp);
         K := Arg_Exp - P_Exp;
         P_Even := True;
         IEEE_Rem := Arg;

         for Cnt in reverse 0 .. UI_To_Int (K) loop
            if IEEE_Rem >= P then
               P_Even := False;
               IEEE_Rem := IEEE_Rem - P;
            else
               P_Even := True;
            end if;

            P := P * Ureal_Half;
         end loop;
      end if;

      --  That completes the calculation of modulus remainder. The final step
      --  is get the IEEE remainder. Here we compare Rem with (abs Y) / 2.

      if P_Exp >= 0 then
         A := IEEE_Rem;
         B := abs Y * Ureal_Half;

      else
         A := IEEE_Rem * Ureal_2;
         B := abs Y;
      end if;

      if A > B or else (A = B and then not P_Even) then
         IEEE_Rem := IEEE_Rem - abs Y;
      end if;

      return Sign_X * IEEE_Rem;

   end Remainder;

   --------------
   -- Rounding --
   --------------

   function Rounding (RT : R; X : T) return T is
      Result : T;
      Tail   : T;

   begin
      Result := Truncation (RT, abs X);
      Tail   := abs X - Result;

      if Tail >= Ureal_Half  then
         Result := Result + Ureal_1;
      end if;

      if UR_Is_Negative (X) then
         return -Result;
      else
         return Result;
      end if;

   end Rounding;

   -------------
   -- Scaling --
   -------------

   function Scaling (RT : R; X : T; Adjustment : UI) return T is
   begin
      if Rbase (X) = Radix then
         return UR_From_Components
           (Num      => Numerator (X),
            Den      => Denominator (X) - Adjustment,
            Rbase    => Radix,
            Negative => UR_Is_Negative (X));

      elsif Adjustment >= 0 then
         return X * Radix ** Adjustment;
      else
         return X / Radix ** (-Adjustment);
      end if;
   end Scaling;

   ----------
   -- Succ --
   ----------

   function Succ (RT : R; X : T) return T is
      Result_F : UI;
      Result_X : UI;

   begin
      if abs X < Eps_Model (RT) then
         if Denorm_On_Target then
            return X + Eps_Denorm (RT);

         elsif X < Ureal_0 then
            --  Target does not support denorms, so successor is 0.0
            return Ureal_0;

         else
            --  Target does not support denorms, and X is 0.0
            --  or at least smaller than Eps_Model (RT)

            return Eps_Model (RT);
         end if;

      else
         Decompose_Int (RT, X, Result_F, Result_X, Floor);
         return UR_From_Components
           (Num      => Result_F + 1,
            Den      => Machine_Mantissa (RT) - Result_X,
            Rbase    => Radix,
            Negative => False);
         --  Result_F may be false, but this is OK as UR_From_Components
         --  handles that situation.
      end if;
   end Succ;

   ----------------
   -- Truncation --
   ----------------

   function Truncation (RT : R; X : T) return T is
   begin
      return UR_From_Uint (UR_Trunc (X));
   end Truncation;

   -----------------------
   -- Unbiased_Rounding --
   -----------------------

   function Unbiased_Rounding (RT : R; X : T) return T is
      Abs_X  : constant T := abs X;
      Result : T;
      Tail   : T;

   begin
      Result := Truncation (RT, Abs_X);
      Tail   := Abs_X - Result;

      if Tail > Ureal_Half  then
         Result := Result + Ureal_1;

      elsif Tail = Ureal_Half then
         Result := Ureal_2 *
                     Truncation (RT, (Result / Ureal_2) + Ureal_Half);
      end if;

      if UR_Is_Negative (X) then
         return -Result;
      elsif UR_Is_Positive (X) then
         return Result;

      --  For zero case, make sure sign of zero is preserved

      else
         return X;
      end if;

   end Unbiased_Rounding;

end Eval_Fat;
