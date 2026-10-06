/* SPDX-License-Identifier: MIT */
/*
 * The Conexant engine walks its own constructor table until it reads a NULL
 * entry. Give the (renamed) table its own output section with an explicit
 * zero terminator instead of relying on alignment padding.
 */
SECTIONS
{
	.hsf.ctors : ALIGN(8)
	{
		KEEP(*(.hsf.hsfengine.ctors))
		QUAD(0)
	}
}
INSERT AFTER .data;
