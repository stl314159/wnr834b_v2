<!--
Copyright 2007, Broadcom Corporation
All Rights Reserved.

THIS SOFTWARE IS OFFERED "AS IS", AND BROADCOM GRANTS NO WARRANTIES OF ANY
KIND, EXPRESS OR IMPLIED, BY STATUTE, COMMUNICATION OR OTHERWISE. BROADCOM
SPECIFICALLY DISCLAIMS ANY IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A SPECIFIC PURPOSE OR NONINFRINGEMENT CONCERNING THIS SOFTWARE.

$Id: internal.asp,v 1.1.1.1 2010/03/05 07:31:37 reynolds Exp $
-->

<!DOCTYPE HTML PUBLIC "-//W3C//DTD HTML 4.01 Transitional//EN">
<html lang="en">
<head>
<title>Broadcom Home Gateway Reference Design: Internal</title>
<meta http-equiv="Content-Type" content="text/html; charset=utf-8">
<link rel="stylesheet" type="text/css" href="style.css" media="screen">
<script language="JavaScript" type="text/javascript" src="overlib.js"></script>
<script language="JavaScript" type="text/javascript">

<!--
function nfs_proto_change() {
/*
*/
}
//-->
</script>
</head>

<body onLoad="nfs_proto_change();">

<div id="overDiv" style="position:absolute; visibility:hidden; z-index:1000;"></div>

<table border="0" cellpadding="0" cellspacing="0" width="100%" bgcolor="#cc0000">
  <% asp_list(); %>
</table>

<table border="0" cellpadding="0" cellspacing="0" width="100%">
  <tr>
    <td colspan="2" class="edge"><img border="0" src="blur_new.jpg" alt=""></td>
  </tr>
  <tr>
    <td><img border="0" src="logo_new.gif" alt=""></td>
    <td width="100%" valign="top">
	<br>
	<span class="title">INTERNAL</span><br>
	<span class="subtitle">This screen is for Broadcom internal
	usage only.</span>
    </td>
  </tr>
</table>

<form method="post" action="internal.asp">
<input type="hidden" name="page" value="internal.asp">

<!--
-->	

<p>
<table border="0" cellpadding="0" cellspacing="0">
  <tr>
    <th width="310"
	onMouseOver="return overlib('Selects which wireless interface to configure.', LEFT);"
	onMouseOut="return nd();">
	Wireless Interface:&nbsp;&nbsp;
    </th>
    <td>&nbsp;&nbsp;</td>
    <td>
	<select name="wl_unit" onChange="submit();">
	  <% wl_list(); %>
	</select>
    </td>
  </tr>
  <tr>
    <th width="310">54g Only Mode:&nbsp;&nbsp;</th>
    <td>&nbsp;&nbsp;</td>
    <td>
	<select name="wl_gmode">
	  <option value="1" <% nvram_invmatch("wl_gmode", "2", "selected"); %>>Disabled</option>
	  <option value="2" <% nvram_match("wl_gmode", "2", "selected"); %>>Enabled</option>
	</select>
    </td>
  </tr>
   <tr>
     <th width="310"
 	onMouseOver="return overlib('Sets whether system log messages will be saved in RAM for showing in web.', LEFT);"
 	onMouseOut="return nd();">
 	Syslog in RAM:&nbsp;&nbsp;
     </th>
     <td>&nbsp;&nbsp;</td>
     <td>
 	<select name="log_ram_enable">
 	  <option value="0" <% nvram_match("log_ram_enable", "0", "selected"); %>>Disabled</option>
 	  <option value="1" <% nvram_match("log_ram_enable", "1", "selected"); %>>Enabled</option>
 	</select>
     </td>
   </tr>
</table>

<!--
-->

<p>
<table border="0" cellpadding="0" cellspacing="0">
  <tr>
    <td width="310"></td>
    <td>&nbsp;&nbsp;</td>
    <td>
	<input type="submit" name="action" value="Apply">
	<input type="reset" name="action" value="Cancel">
	<input type="submit" name="action" value="Upgrade">
	<input type="submit" name="action" value="Stats">
	<% wl_radio_roam_option(); %>
    </td>
  </tr>
</table>

</form>

<p class="label">&#169;2001-2007 Broadcom Corporation. All rights reserved. 

</body>
</html>
