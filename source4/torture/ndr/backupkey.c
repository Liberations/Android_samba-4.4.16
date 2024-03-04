/*
   Unix SMB/CIFS implementation.
   Test suite for ndr on backupkey

   Copyright (C) Matthieu Patou 2010

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "includes.h"
#include "torture/ndr/ndr.h"
#include "librpc/gen_ndr/ndr_backupkey.h"
#include "torture/ndr/proto.h"


static const uint8_t exported_rsa_ndr[] = {
0x02, 0x00, 0x00, 0x00, 0x94, 0x04, 0x00, 0x00, 0x04, 0x03, 0x00, 0x00, 0x07, 0x02, 0x00, 0x00,
0x00, 0xa4, 0x00, 0x00, 0x52, 0x53, 0x41, 0x32, 0x00, 0x08, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00,
0x21, 0x5d, 0x57, 0xa5, 0x84, 0xfc, 0xf4, 0x89, 0x47, 0x27, 0xfe, 0x71, 0xc1, 0x7b, 0x27, 0xb1,
0xfd, 0xe4, 0x1d, 0x7d, 0x0f, 0xe8, 0xdd, 0xc1, 0xe2, 0x95, 0x4e, 0x4a, 0xdb, 0xc7, 0x27, 0x31,
0xbd, 0x22, 0xd7, 0xfe, 0x42, 0x12, 0x94, 0x17, 0x5e, 0x94, 0x0e, 0xc6, 0xa5, 0xef, 0xf8, 0x20,
0x0a, 0x0f, 0xc7, 0xbf, 0x79, 0x1d, 0x12, 0x98, 0x7a, 0x21, 0xfc, 0x37, 0x9a, 0xe6, 0xec, 0x02,
0xf6, 0xdf, 0x08, 0x49, 0x62, 0xd2, 0xee, 0x37, 0xe4, 0x1c, 0x95, 0x05, 0xd2, 0xdf, 0x8b, 0x85,
0x80, 0x5f, 0xcb, 0x3f, 0x70, 0xb9, 0xf1, 0xf3, 0x4f, 0x8f, 0x6d, 0x73, 0xc9, 0x15, 0x2c, 0x97,
0xb2, 0x2c, 0x17, 0x6d, 0x8b, 0xe7, 0x9a, 0x58, 0x2c, 0xcb, 0xd0, 0x06, 0x5f, 0x48, 0x49, 0xf8,
0x80, 0x13, 0x25, 0x05, 0x2a, 0x5a, 0x71, 0x01, 0x39, 0x36, 0x89, 0x80, 0x70, 0xdb, 0x57, 0x1a,
0xe3, 0xd7, 0xcc, 0xbd, 0x9d, 0x1f, 0x1d, 0x92, 0x60, 0x63, 0x78, 0x57, 0xd0, 0x36, 0x42, 0x07,
0x7c, 0xdc, 0x58, 0x32, 0x6c, 0xa6, 0xfd, 0xc0, 0x85, 0x19, 0x5f, 0x32, 0x7b, 0xd6, 0x40, 0xa9,
0xf5, 0x1a, 0x9f, 0xec, 0x7a, 0x59, 0x10, 0x71, 0xaa, 0x22, 0x39, 0x34, 0xe1, 0xd3, 0x5b, 0x0f,
0x39, 0xd8, 0x57, 0xba, 0x59, 0x5f, 0xf3, 0xdd, 0x4d, 0x36, 0xde, 0xdc, 0x2c, 0xd3, 0x30, 0x6b,
0x55, 0xaa, 0x5a, 0x51, 0xf6, 0xef, 0x42, 0x5c, 0x01, 0x26, 0x2b, 0x42, 0xd3, 0xf4, 0x9c, 0x6b,
0x56, 0xb0, 0xd3, 0x80, 0x77, 0xb5, 0x80, 0xf3, 0x89, 0xbf, 0xc3, 0xd7, 0x71, 0x2e, 0x47, 0x3e,
0x86, 0x84, 0xec, 0x9f, 0xa0, 0x38, 0xbb, 0xe9, 0xce, 0x34, 0x7d, 0x9e, 0xf7, 0xf5, 0xe8, 0xfd,
0x90, 0x5b, 0xc1, 0x97, 0x2b, 0x08, 0x55, 0x4a, 0x57, 0x69, 0xfc, 0xd7, 0x26, 0x32, 0xd7, 0xaa,
0x7d, 0x66, 0x4f, 0x4d, 0x46, 0xcb, 0xc9, 0x83, 0x92, 0xd9, 0x56, 0xac, 0xb0, 0x5c, 0x0a, 0xd9,
0xeb, 0x38, 0xae, 0x24, 0x9a, 0xb0, 0x7d, 0x3c, 0x56, 0x0b, 0xbf, 0xca, 0xa9, 0xbc, 0x75, 0xad,
0x27, 0x2f, 0x9d, 0x16, 0xb5, 0xe5, 0xf3, 0xac, 0x5e, 0x0d, 0xe1, 0x9f, 0x67, 0xb9, 0x8d, 0xeb,
0x8a, 0xea, 0x2a, 0x00, 0xa5, 0x09, 0x35, 0x7b, 0xcc, 0xeb, 0x00, 0xdb, 0x46, 0x49, 0xac, 0x3c,
0x00, 0x7a, 0x1e, 0x31, 0x21, 0x66, 0x7b, 0xe6, 0x10, 0x04, 0x2b, 0x39, 0x21, 0xa9, 0xe3, 0xa0,
0x81, 0x9e, 0xeb, 0xbe, 0x04, 0xe6, 0xd4, 0x23, 0xdc, 0xca, 0x01, 0xd3, 0xfa, 0x73, 0xba, 0x75,
0x67, 0x61, 0x7d, 0x95, 0x12, 0x14, 0x4d, 0x1e, 0x08, 0x60, 0x0f, 0xbb, 0x06, 0xcf, 0xdc, 0x1c,
0x07, 0x3c, 0x61, 0xd9, 0x54, 0xc8, 0xca, 0x31, 0x1c, 0x5d, 0xc4, 0xf1, 0x01, 0x59, 0xfd, 0xdb,
0x75, 0x7e, 0xcc, 0xd0, 0xe7, 0x0a, 0x9a, 0x36, 0x1e, 0xc5, 0xf9, 0x95, 0x83, 0x4d, 0x3e, 0x47,
0x2e, 0x70, 0x12, 0x4e, 0x0e, 0x07, 0xe4, 0x56, 0x6f, 0xe7, 0xed, 0xbd, 0xe4, 0x48, 0x50, 0x0b,
0xa6, 0x49, 0x80, 0x1c, 0x85, 0xe7, 0x92, 0x02, 0x4d, 0xfa, 0xe3, 0x6d, 0x1f, 0xc1, 0xf7, 0xf9,
0xd0, 0x37, 0x68, 0x33, 0x85, 0x20, 0x71, 0x9c, 0xc2, 0xaa, 0x58, 0x47, 0xae, 0x39, 0x1b, 0x20,
0x3f, 0xc1, 0x90, 0x7c, 0x3d, 0xee, 0xc9, 0x9e, 0x1b, 0x07, 0xd3, 0x0a, 0x13, 0xd1, 0xca, 0x53,
0xd0, 0x2a, 0xf6, 0x2c, 0xa9, 0xd8, 0xef, 0xe3, 0xe0, 0x24, 0x3f, 0xf9, 0x13, 0xe3, 0xf1, 0xff,
0x7b, 0x47, 0x59, 0x09, 0xd5, 0x6d, 0x52, 0x95, 0x87, 0xcb, 0x44, 0x6e, 0xe8, 0xaa, 0x0c, 0xe4,
0xd1, 0xb3, 0xc9, 0xd8, 0xc0, 0xcf, 0x2b, 0x53, 0xf7, 0x8e, 0x93, 0xe9, 0xd8, 0x42, 0xce, 0xc6,
0x01, 0x67, 0x73, 0x44, 0x5a, 0x65, 0x36, 0x96, 0x9f, 0xd1, 0xfc, 0x03, 0x9b, 0xd1, 0x0d, 0x02,
0xc7, 0x1e, 0xe4, 0x93, 0xa6, 0x00, 0x6e, 0xfe, 0x6f, 0x8e, 0xf1, 0x93, 0x97, 0xdf, 0x28, 0xee,
0x3e, 0x07, 0xa1, 0x38, 0xe1, 0x1b, 0xa8, 0x77, 0x78, 0x0d, 0x86, 0x39, 0x20, 0xcb, 0x71, 0xd8,
0x2a, 0x63, 0x63, 0x00, 0xdf, 0x6e, 0xea, 0x44, 0xf1, 0xdc, 0x2e, 0xf0, 0x81, 0x41, 0x92, 0x55,
0xce, 0x2d, 0x7c, 0xc4, 0x7f, 0x94, 0xef, 0xc9, 0xe4, 0x26, 0x15, 0x7e, 0x59, 0x9b, 0xd0, 0x10,
0x64, 0xa6, 0x2e, 0xf7, 0x71, 0xcc, 0x5b, 0xcd, 0xa2, 0xf3, 0xe7, 0xe5, 0x56, 0xd0, 0x9d, 0xd0,
0xaf, 0x31, 0xfc, 0x08, 0xd6, 0xff, 0x87, 0xca, 0x27, 0xa9, 0xf0, 0xf1, 0xf8, 0x2d, 0x8d, 0x1c,
0xce, 0x48, 0xc9, 0x60, 0x50, 0x58, 0x56, 0x31, 0x6f, 0xa0, 0xbf, 0x8d, 0xf5, 0xcd, 0x3e, 0x0c,
0x11, 0xfc, 0x5c, 0x0b, 0xe3, 0x77, 0x9e, 0x44, 0x1e, 0x1d, 0xda, 0x67, 0x8a, 0x40, 0xf2, 0x89,
0x94, 0xd5, 0xec, 0xc9, 0xf6, 0xe8, 0xc5, 0x81, 0xd9, 0x28, 0x8b, 0x3f, 0x13, 0xc5, 0x0e, 0x3d,
0x7f, 0x81, 0x97, 0xac, 0x52, 0xee, 0xbb, 0xcb, 0xbb, 0x3d, 0x86, 0x35, 0x1f, 0x31, 0x1a, 0x97,
0x54, 0xf5, 0x04, 0xb4, 0x24, 0x21, 0x4e, 0xea, 0x05, 0xf3, 0x6d, 0x08, 0x20, 0xb5, 0xf7, 0x37,
0xf6, 0xd3, 0xff, 0x57, 0x7e, 0x85, 0xf1, 0x66, 0xba, 0xed, 0xf1, 0xe9, 0xbd, 0x5b, 0x91, 0xb2,
0x89, 0xe8, 0xdb, 0xac, 0x5b, 0x58, 0xa9, 0x0f, 0x03, 0x18, 0x38, 0x55, 0x80, 0x19, 0x04, 0x0d,
0xa8, 0x91, 0x8b, 0x3c, 0x65, 0x23, 0x78, 0xbc, 0x0e, 0x5b, 0xc5, 0x80, 0x6e, 0xad, 0x1f, 0x97,
0x28, 0xe5, 0x57, 0xff, 0xc9, 0x06, 0x7d, 0xa8, 0xbe, 0x65, 0xfc, 0xcd, 0x99, 0x04, 0x3a, 0x36,
0xe3, 0xe8, 0x41, 0xd5, 0x9b, 0x13, 0x98, 0xf6, 0x76, 0xfb, 0x23, 0x6b, 0x9f, 0x2b, 0xdb, 0x06,
0x25, 0xf3, 0x72, 0x33, 0x35, 0x92, 0x51, 0xb6, 0x49, 0x98, 0xee, 0x48, 0xc8, 0xad, 0x7b, 0x87,
0x4a, 0x3d, 0x86, 0x69, 0x1b, 0xd3, 0x15, 0xe3, 0x6c, 0xe9, 0x83, 0x73, 0x3b, 0x0f, 0x0d, 0x0e,
0xe2, 0x9c, 0xfe, 0xe6, 0xc0, 0x4d, 0xb9, 0xe4, 0x89, 0x56, 0x9d, 0xc0, 0x7a, 0x0c, 0xed, 0x1a,
0x70, 0x1f, 0x2c, 0x73, 0x05, 0x22, 0x19, 0x2c, 0x70, 0x94, 0x73, 0x3d, 0x91, 0xee, 0x2d, 0xff,
0x9c, 0x50, 0x94, 0x7b, 0x85, 0xaa, 0x42, 0xc0, 0xc9, 0xbf, 0xdc, 0xc5, 0x29, 0xaf, 0xca, 0x93,
0x43, 0xcc, 0xc8, 0x0c, 0x3e, 0x91, 0xce, 0xcd, 0x93, 0xe6, 0x0f, 0x76, 0xcc, 0x56, 0x7a, 0x44,
0x7c, 0x9d, 0x6b, 0xb6, 0x2d, 0xf7, 0x01, 0x3a, 0x72, 0xfb, 0x85, 0x2a, 0x37, 0xf2, 0x33, 0x0c,
0xc1, 0x2a, 0xb4, 0x6b, 0x4f, 0xdf, 0xcd, 0x78, 0xf8, 0x18, 0xd6, 0x1e, 0xb9, 0x2e, 0x17, 0xf5,
0xcb, 0x0e, 0xca, 0xc3, 0xf0, 0x5b, 0x2d, 0x61, 0x7b, 0xef, 0x37, 0xd7, 0x35, 0xf7, 0x90, 0x30,
0x64, 0x46, 0x43, 0x94, 0x56, 0x5b, 0xd1, 0x10, 0x10, 0xae, 0xa4, 0x20, 0xce, 0xb3, 0x91, 0x31,
0xbc, 0x06, 0xf2, 0xbc, 0xa0, 0x66, 0x52, 0xb5, 0xd3, 0x51, 0x6e, 0x24, 0x63, 0x3d, 0xaa, 0xa9,
0xa9, 0x8c, 0x74, 0xf3, 0x09, 0xbf, 0x01, 0x3f, 0x48, 0x0e, 0x4a, 0x87, 0x3d, 0x91, 0x96, 0x33,
0x17, 0x4d, 0x43, 0xe5, 0x71, 0x2c, 0x94, 0x64, 0x39, 0xe9, 0xdb, 0xdb, 0x05, 0x5f, 0x07, 0x38,
0xa3, 0x36, 0x7b, 0x79, 0x9a, 0x74, 0xf7, 0x0e, 0x15, 0x9f, 0x49, 0x65, 0x2d, 0xf5, 0x85, 0x6c,
0xc8, 0xbc, 0x42, 0x88, 0x93, 0xd9, 0x40, 0xfa, 0xbf, 0x14, 0x66, 0x68, 0x9e, 0x05, 0x64, 0x38,
0x4b, 0xb5, 0x97, 0x2c, 0x48, 0x90, 0x09, 0x8e, 0x60, 0xc0, 0x56, 0xd6, 0x44, 0x2f, 0x60, 0xe8,
0x0f, 0xa5, 0xf3, 0x95, 0xca, 0x9a, 0x09, 0x05, 0x8a, 0x3d, 0xaf, 0x01, 0x71, 0x66, 0x68, 0xa5,
0x06, 0x6e, 0x95, 0x33, 0x46, 0x9d, 0x45, 0xcb, 0x2c, 0xdd, 0x05, 0x8d, 0xbb, 0x8f, 0xa7, 0x5b,
0xec, 0x0b, 0x17, 0x54, 0xe3, 0xd0, 0x7d, 0xb9, 0x23, 0x77, 0xf3, 0xc6, 0x3e, 0x69, 0x2a, 0xf9,
0xe9, 0xcf, 0x83, 0xc4, 0x09, 0xa5, 0x2a, 0xd9, 0xb3, 0x4e, 0x3f, 0x16, 0x7d, 0xf7, 0x8f, 0xb3,
0xd0, 0x64, 0x2b, 0xc3, 0x0e, 0xb1, 0xf3, 0xdb, 0xe6, 0x4a, 0x7e, 0xf9, 0x3b, 0xc9, 0xca, 0x14,
0x9d, 0xf2, 0x61, 0xc3, 0x59, 0x05, 0xcf, 0x0d, 0x13, 0xcf, 0x2e, 0xa9, 0xa6, 0x59, 0x30, 0xa6,
0xef, 0x3a, 0x43, 0xbb, 0x63, 0x6f, 0x31, 0x7a, 0xfa, 0x38, 0x0d, 0xb1, 0x17, 0x4d, 0xc2, 0xa4,
0x30, 0x82, 0x03, 0x00, 0x30, 0x82, 0x01, 0xec, 0xa0, 0x03, 0x02, 0x01, 0x02, 0x02, 0x10, 0xbd,
0x76, 0xdf, 0x42, 0x47, 0x0a, 0x00, 0x8d, 0x47, 0x3e, 0x74, 0x3f, 0xa1, 0xdc, 0x8b, 0xbd, 0x30,
0x09, 0x06, 0x05, 0x2b, 0x0e, 0x03, 0x02, 0x1d, 0x05, 0x00, 0x30, 0x2d, 0x31, 0x2b, 0x30, 0x29,
0x06, 0x03, 0x55, 0x04, 0x03, 0x13, 0x22, 0x77, 0x00, 0x32, 0x00, 0x6b, 0x00, 0x38, 0x00, 0x72,
0x00, 0x32, 0x00, 0x2e, 0x00, 0x6d, 0x00, 0x61, 0x00, 0x74, 0x00, 0x77, 0x00, 0x73, 0x00, 0x2e,
0x00, 0x6e, 0x00, 0x65, 0x00, 0x74, 0x00, 0x00, 0x00, 0x30, 0x1e, 0x17, 0x0d, 0x31, 0x30, 0x30,
0x34, 0x32, 0x38, 0x31, 0x31, 0x34, 0x31, 0x35, 0x34, 0x5a, 0x17, 0x0d, 0x31, 0x31, 0x30, 0x34,
0x32, 0x38, 0x31, 0x31, 0x34, 0x31, 0x35, 0x34, 0x5a, 0x30, 0x2d, 0x31, 0x2b, 0x30, 0x29, 0x06,
0x03, 0x55, 0x04, 0x03, 0x13, 0x22, 0x77, 0x00, 0x32, 0x00, 0x6b, 0x00, 0x38, 0x00, 0x72, 0x00,
0x32, 0x00, 0x2e, 0x00, 0x6d, 0x00, 0x61, 0x00, 0x74, 0x00, 0x77, 0x00, 0x73, 0x00, 0x2e, 0x00,
0x6e, 0x00, 0x65, 0x00, 0x74, 0x00, 0x00, 0x00, 0x30, 0x82, 0x01, 0x22, 0x30, 0x0d, 0x06, 0x09,
0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01, 0x05, 0x00, 0x03, 0x82, 0x01, 0x0f, 0x00,
0x30, 0x82, 0x01, 0x0a, 0x02, 0x82, 0x01, 0x01, 0x00, 0xaa, 0xd7, 0x32, 0x26, 0xd7, 0xfc, 0x69,
0x57, 0x4a, 0x55, 0x08, 0x2b, 0x97, 0xc1, 0x5b, 0x90, 0xfd, 0xe8, 0xf5, 0xf7, 0x9e, 0x7d, 0x34,
0xce, 0xe9, 0xbb, 0x38, 0xa0, 0x9f, 0xec, 0x84, 0x86, 0x3e, 0x47, 0x2e, 0x71, 0xd7, 0xc3, 0xbf,
0x89, 0xf3, 0x80, 0xb5, 0x77, 0x80, 0xd3, 0xb0, 0x56, 0x6b, 0x9c, 0xf4, 0xd3, 0x42, 0x2b, 0x26,
0x01, 0x5c, 0x42, 0xef, 0xf6, 0x51, 0x5a, 0xaa, 0x55, 0x6b, 0x30, 0xd3, 0x2c, 0xdc, 0xde, 0x36,
0x4d, 0xdd, 0xf3, 0x5f, 0x59, 0xba, 0x57, 0xd8, 0x39, 0x0f, 0x5b, 0xd3, 0xe1, 0x34, 0x39, 0x22,
0xaa, 0x71, 0x10, 0x59, 0x7a, 0xec, 0x9f, 0x1a, 0xf5, 0xa9, 0x40, 0xd6, 0x7b, 0x32, 0x5f, 0x19,
0x85, 0xc0, 0xfd, 0xa6, 0x6c, 0x32, 0x58, 0xdc, 0x7c, 0x07, 0x42, 0x36, 0xd0, 0x57, 0x78, 0x63,
0x60, 0x92, 0x1d, 0x1f, 0x9d, 0xbd, 0xcc, 0xd7, 0xe3, 0x1a, 0x57, 0xdb, 0x70, 0x80, 0x89, 0x36,
0x39, 0x01, 0x71, 0x5a, 0x2a, 0x05, 0x25, 0x13, 0x80, 0xf8, 0x49, 0x48, 0x5f, 0x06, 0xd0, 0xcb,
0x2c, 0x58, 0x9a, 0xe7, 0x8b, 0x6d, 0x17, 0x2c, 0xb2, 0x97, 0x2c, 0x15, 0xc9, 0x73, 0x6d, 0x8f,
0x4f, 0xf3, 0xf1, 0xb9, 0x70, 0x3f, 0xcb, 0x5f, 0x80, 0x85, 0x8b, 0xdf, 0xd2, 0x05, 0x95, 0x1c,
0xe4, 0x37, 0xee, 0xd2, 0x62, 0x49, 0x08, 0xdf, 0xf6, 0x02, 0xec, 0xe6, 0x9a, 0x37, 0xfc, 0x21,
0x7a, 0x98, 0x12, 0x1d, 0x79, 0xbf, 0xc7, 0x0f, 0x0a, 0x20, 0xf8, 0xef, 0xa5, 0xc6, 0x0e, 0x94,
0x5e, 0x17, 0x94, 0x12, 0x42, 0xfe, 0xd7, 0x22, 0xbd, 0x31, 0x27, 0xc7, 0xdb, 0x4a, 0x4e, 0x95,
0xe2, 0xc1, 0xdd, 0xe8, 0x0f, 0x7d, 0x1d, 0xe4, 0xfd, 0xb1, 0x27, 0x7b, 0xc1, 0x71, 0xfe, 0x27,
0x47, 0x89, 0xf4, 0xfc, 0x84, 0xa5, 0x57, 0x5d, 0x21, 0x02, 0x03, 0x01, 0x00, 0x01, 0x81, 0x11,
0x00, 0xbd, 0x8b, 0xdc, 0xa1, 0x3f, 0x74, 0x3e, 0x47, 0x8d, 0x00, 0x0a, 0x47, 0x42, 0xdf, 0x76,
0xbd, 0x82, 0x11, 0x00, 0xbd, 0x8b, 0xdc, 0xa1, 0x3f, 0x74, 0x3e, 0x47, 0x8d, 0x00, 0x0a, 0x47,
0x42, 0xdf, 0x76, 0xbd, 0x30, 0x09, 0x06, 0x05, 0x2b, 0x0e, 0x03, 0x02, 0x1d, 0x05, 0x00, 0x03,
0x82, 0x01, 0x01, 0x00, 0xa7, 0xb0, 0x66, 0x75, 0x14, 0x7e, 0x7d, 0xb5, 0x31, 0xec, 0xb2, 0xeb,
0x90, 0x80, 0x95, 0x25, 0x59, 0x0f, 0xe4, 0x15, 0x86, 0x2d, 0x9d, 0xd7, 0x35, 0xe9, 0x22, 0x74,
0xe7, 0x85, 0x36, 0x19, 0x4f, 0x27, 0x5c, 0x17, 0x63, 0x7b, 0x2a, 0xfe, 0x59, 0xe9, 0x76, 0x77,
0xd0, 0xc9, 0x40, 0x78, 0x7c, 0x31, 0x62, 0x1e, 0x87, 0x1b, 0xc1, 0x19, 0xef, 0x6f, 0x15, 0xe6,
0xce, 0x74, 0x84, 0x6d, 0xd6, 0x3b, 0x57, 0xd9, 0xa9, 0x13, 0xf6, 0x7d, 0x84, 0xe7, 0x8f, 0xc6,
0x01, 0x5f, 0xcf, 0xc4, 0x95, 0xc9, 0xde, 0x97, 0x17, 0x43, 0x12, 0x70, 0x27, 0xf9, 0xc4, 0xd7,
0xe1, 0x05, 0xbb, 0x63, 0x87, 0x5f, 0xdc, 0x20, 0xbd, 0xd1, 0xde, 0xd6, 0x2d, 0x9f, 0x3f, 0x5d,
0x0a, 0x27, 0x40, 0x11, 0x5f, 0x5d, 0x54, 0xa7, 0x28, 0xf9, 0x03, 0x2e, 0x84, 0x8d, 0x48, 0x60,
0xa1, 0x71, 0xa3, 0x46, 0x69, 0xdb, 0x88, 0x7b, 0xc1, 0xb6, 0x08, 0x2d, 0xdf, 0x25, 0x9d, 0x32,
0x76, 0x49, 0x0b, 0xba, 0xab, 0xdd, 0xc3, 0x00, 0x76, 0x8a, 0x94, 0xd2, 0x25, 0x43, 0xf0, 0xa9,
0x98, 0x65, 0x94, 0xc7, 0xdd, 0x7c, 0xd4, 0xe2, 0xe8, 0x33, 0xe2, 0x9a, 0xe9, 0x75, 0xf0, 0x0f,
0x61, 0x86, 0xee, 0x0e, 0xf7, 0x39, 0x6b, 0x30, 0x63, 0xe5, 0x46, 0xd4, 0x1c, 0x83, 0xa1, 0x28,
0x79, 0x76, 0x81, 0x48, 0x38, 0x72, 0xbc, 0x3f, 0x25, 0x53, 0x31, 0xaa, 0x02, 0xd1, 0x9b, 0x03,
0xa2, 0x5c, 0x94, 0x21, 0xb3, 0x8e, 0xdf, 0x2a, 0xa5, 0x4c, 0x65, 0xa2, 0xf9, 0xac, 0x38, 0x7a,
0xf9, 0x45, 0xb3, 0xd5, 0xda, 0xe5, 0xb9, 0x56, 0x9e, 0x47, 0xd5, 0x06, 0xe6, 0xca, 0xd7, 0x6e,
0x06, 0xdb, 0x6e, 0xa7, 0x7b, 0x4b, 0x13, 0x40, 0x3c, 0x12, 0x76, 0x99, 0x65, 0xb4, 0x54, 0xa1,
0xd8, 0x21, 0x5c, 0x27
};

struct torture_suite *ndr_backupkey_suite(TALLOC_CTX *ctx)
{
	struct torture_suite *suite = torture_suite_create(ctx, "backupkey");

	torture_suite_add_ndr_pullpush_test(suite,
					    bkrp_exported_RSA_key_pair,
					    data_blob_const(exported_rsa_ndr,
							    sizeof(exported_rsa_ndr)),
					    NULL);

	return suite;
}
