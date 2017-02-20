//-----------------------------------------------------------------------------
// Copyright (C) 2010 iZsh <izsh at fail0verflow.com>
//
// This code is licensed to you under the terms of the GNU GPL, version 2 or,
// at your option, any later version. See the LICENSE.txt file for the text of
// the license.
//-----------------------------------------------------------------------------
// Low frequency EM4x commands
//-----------------------------------------------------------------------------

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "proxmark3.h"
#include "ui.h"
#include "util.h"
#include "graph.h"
#include "cmdparser.h"
#include "cmddata.h"
#include "cmdlf.h"
#include "cmdmain.h"
#include "cmdlfem4x.h"
#include "lfdemod.h"

char *global_em410xId;

static int CmdHelp(const char *Cmd);

int CmdEMdemodASK(const char *Cmd)
{
	char cmdp = param_getchar(Cmd, 0);
	int findone = (cmdp == '1') ? 1 : 0;
	UsbCommand c={CMD_EM410X_DEMOD};
	c.arg[0]=findone;
	SendCommand(&c);
	return 0;
}

/* Read the ID of an EM410x tag.
 * Format:
 *   1111 1111 1           <-- standard non-repeatable header
 *   XXXX [row parity bit] <-- 10 rows of 5 bits for our 40 bit tag ID
 *   ....
 *   CCCC                  <-- each bit here is parity for the 10 bits above in corresponding column
 *   0                     <-- stop bit, end of tag
 */
int CmdEM410xRead(const char *Cmd)
{
	uint32_t hi=0;
	uint64_t lo=0;

	if(!AskEm410xDemod("", &hi, &lo, false)) return 0;
	PrintAndLog("EM410x pattern found: ");
	printEM410x(hi, lo);
	if (hi){
		PrintAndLog ("EM410x XL pattern found");
		return 0;
	}
	char id[12] = {0x00};
	sprintf(id, "%010"PRIx64,lo);
	
	global_em410xId = id;
	return 1;
}

// emulate an EM410X tag
int CmdEM410xSim(const char *Cmd)
{
	int i, n, j, binary[4], parity[4];

	char cmdp = param_getchar(Cmd, 0);
	uint8_t uid[5] = {0x00};

	if (cmdp == 'h' || cmdp == 'H') {
		PrintAndLog("Usage:  lf em4x em410xsim <UID> <clock>");
		PrintAndLog("");
		PrintAndLog("     sample: lf em4x em410xsim 0F0368568B");
		return 0;
	}
	/* clock is 64 in EM410x tags */
	uint8_t clock = 64;

	if (param_gethex(Cmd, 0, uid, 10)) {
		PrintAndLog("UID must include 10 HEX symbols");
		return 0;
	}
	param_getdec(Cmd,1, &clock);

	PrintAndLog("Starting simulating UID %02X%02X%02X%02X%02X  clock: %d", uid[0],uid[1],uid[2],uid[3],uid[4],clock);
	PrintAndLog("Press pm3-button to about simulation");


	/* clear our graph */
	ClearGraph(0);

		/* write 9 start bits */
		for (i = 0; i < 9; i++)
			AppendGraph(0, clock, 1);

		/* for each hex char */
		parity[0] = parity[1] = parity[2] = parity[3] = 0;
		for (i = 0; i < 10; i++)
		{
			/* read each hex char */
			sscanf(&Cmd[i], "%1x", &n);
			for (j = 3; j >= 0; j--, n/= 2)
				binary[j] = n % 2;

			/* append each bit */
			AppendGraph(0, clock, binary[0]);
			AppendGraph(0, clock, binary[1]);
			AppendGraph(0, clock, binary[2]);
			AppendGraph(0, clock, binary[3]);

			/* append parity bit */
			AppendGraph(0, clock, binary[0] ^ binary[1] ^ binary[2] ^ binary[3]);

			/* keep track of column parity */
			parity[0] ^= binary[0];
			parity[1] ^= binary[1];
			parity[2] ^= binary[2];
			parity[3] ^= binary[3];
		}

		/* parity columns */
		AppendGraph(0, clock, parity[0]);
		AppendGraph(0, clock, parity[1]);
		AppendGraph(0, clock, parity[2]);
		AppendGraph(0, clock, parity[3]);

		/* stop bit */
	AppendGraph(1, clock, 0);
 
	CmdLFSim("0"); //240 start_gap.
	return 0;
}

/* Function is equivalent of lf read + data samples + em410xread
 * looped until an EM410x tag is detected 
 * 
 * Why is CmdSamples("16000")?
 *  TBD: Auto-grow sample size based on detected sample rate.  IE: If the
 *       rate gets lower, then grow the number of samples
 *  Changed by martin, 4000 x 4 = 16000, 
 *  see http://www.proxmark.org/forum/viewtopic.php?pid=7235#p7235
*/
int CmdEM410xWatch(const char *Cmd)
{
	do {
		if (ukbhit()) {
			printf("\naborted via keyboard!\n");
			break;
		}
		
		CmdLFRead("s");
		getSamples("8201",true); //capture enough to get 2 complete preambles (4096*2+9)	
	} while (!CmdEM410xRead(""));

	return 0;
}

//currently only supports manchester modulations
int CmdEM410xWatchnSpoof(const char *Cmd)
{
	CmdEM410xWatch(Cmd);
	PrintAndLog("# Replaying captured ID: %s",global_em410xId);
	CmdLFaskSim("");
	return 0;
}

int CmdEM410xWrite(const char *Cmd)
{
	uint64_t id = 0xFFFFFFFFFFFFFFFF; // invalid id value
	int card = 0xFF; // invalid card value
	unsigned int clock = 0; // invalid clock value

	sscanf(Cmd, "%" PRIx64 " %d %d", &id, &card, &clock);

	// Check ID
	if (id == 0xFFFFFFFFFFFFFFFF) {
		PrintAndLog("Error! ID is required.\n");
		return 0;
	}
	if (id >= 0x10000000000) {
		PrintAndLog("Error! Given EM410x ID is longer than 40 bits.\n");
		return 0;
	}

	// Check Card
	if (card == 0xFF) {
		PrintAndLog("Error! Card type required.\n");
		return 0;
	}
	if (card < 0) {
		PrintAndLog("Error! Bad card type selected.\n");
		return 0;
	}

	// Check Clock
	// Default: 64
	if (clock == 0)
		clock = 64;

	// Allowed clock rates: 16, 32, 40 and 64
	if ((clock != 16) && (clock != 32) && (clock != 64) && (clock != 40)) {
		PrintAndLog("Error! Clock rate %d not valid. Supported clock rates are 16, 32, 40 and 64.\n", clock);
		return 0;
	}

	if (card == 1) {
		PrintAndLog("Writing %s tag with UID 0x%010" PRIx64 " (clock rate: %d)", "T55x7", id, clock);
		// NOTE: We really should pass the clock in as a separate argument, but to
		//   provide for backwards-compatibility for older firmware, and to avoid
		//   having to add another argument to CMD_EM410X_WRITE_TAG, we just store
		//   the clock rate in bits 8-15 of the card value
		card = (card & 0xFF) | ((clock << 8) & 0xFF00);
	}	else if (card == 0) {
		PrintAndLog("Writing %s tag with UID 0x%010" PRIx64, "T5555", id, clock);
		card = (card & 0xFF) | ((clock << 8) & 0xFF00);
	} else {
		PrintAndLog("Error! Bad card type selected.\n");
		return 0;
	}

	UsbCommand c = {CMD_EM410X_WRITE_TAG, {card, (uint32_t)(id >> 32), (uint32_t)id}};
	SendCommand(&c);

	return 0;
}

//**************** Start of EM4x50 Code ************************
bool EM_EndParityTest(uint8_t *BitStream, size_t size, uint8_t rows, uint8_t cols, uint8_t pType)
{
	if (rows*cols>size) return false;
	uint8_t colP=0;
	//assume last col is a parity and do not test
	for (uint8_t colNum = 0; colNum < cols-1; colNum++) {
		for (uint8_t rowNum = 0; rowNum < rows; rowNum++) {
			colP ^= BitStream[(rowNum*cols)+colNum];
		}
		if (colP != pType) return false;
	}
	return true;
}

bool EM_ByteParityTest(uint8_t *BitStream, size_t size, uint8_t rows, uint8_t cols, uint8_t pType)
{
	if (rows*cols>size) return false;
	uint8_t rowP=0;
	//assume last row is a parity row and do not test
	for (uint8_t rowNum = 0; rowNum < rows-1; rowNum++) {
		for (uint8_t colNum = 0; colNum < cols; colNum++) {
			rowP ^= BitStream[(rowNum*cols)+colNum];
		}
		if (rowP != pType) return false;
	}
	return true;
}

uint32_t OutputEM4x50_Block(uint8_t *BitStream, size_t size, bool verbose, bool pTest)
{
	if (size<45) return 0;
	uint32_t code = bytebits_to_byte(BitStream,8);
	code = code<<8 | bytebits_to_byte(BitStream+9,8);
	code = code<<8 | bytebits_to_byte(BitStream+18,8);
	code = code<<8 | bytebits_to_byte(BitStream+27,8);
	if (verbose || g_debugMode){
		for (uint8_t i = 0; i<5; i++){
			if (i == 4) PrintAndLog(""); //parity byte spacer
			PrintAndLog("%d%d%d%d%d%d%d%d %d -> 0x%02x",
			    BitStream[i*9],
			    BitStream[i*9+1],
			    BitStream[i*9+2],
			    BitStream[i*9+3],
			    BitStream[i*9+4],
			    BitStream[i*9+5],
			    BitStream[i*9+6],
			    BitStream[i*9+7],
			    BitStream[i*9+8],
			    bytebits_to_byte(BitStream+i*9,8)
			);
		}
		if (pTest)
			PrintAndLog("Parity Passed");
		else
			PrintAndLog("Parity Failed");
	}
	return code;
}
/* Read the transmitted data of an EM4x50 tag from the graphbuffer
 * Format:
 *
 *  XXXXXXXX [row parity bit (even)] <- 8 bits plus parity
 *  XXXXXXXX [row parity bit (even)] <- 8 bits plus parity
 *  XXXXXXXX [row parity bit (even)] <- 8 bits plus parity
 *  XXXXXXXX [row parity bit (even)] <- 8 bits plus parity
 *  CCCCCCCC                         <- column parity bits
 *  0                                <- stop bit
 *  LW                               <- Listen Window
 *
 * This pattern repeats for every block of data being transmitted.
 * Transmission starts with two Listen Windows (LW - a modulated
 * pattern of 320 cycles each (32/32/128/64/64)).
 *
 * Note that this data may or may not be the UID. It is whatever data
 * is stored in the blocks defined in the control word First and Last
 * Word Read values. UID is stored in block 32.
 */
 //completed by Marshmellow
int EM4x50Read(const char *Cmd, bool verbose)
{
	uint8_t fndClk[] = {8,16,32,40,50,64,128};
	int clk = 0; 
	int invert = 0;
	int tol = 0;
	int i, j, startblock, skip, block, start, end, low, high, minClk;
	bool complete = false;
	int tmpbuff[MAX_GRAPH_TRACE_LEN / 64];
	uint32_t Code[6];
	char tmp[6];
	char tmp2[20];
	int phaseoff;
	high = low = 0;
	memset(tmpbuff, 0, MAX_GRAPH_TRACE_LEN / 64);

	// get user entry if any
	sscanf(Cmd, "%i %i", &clk, &invert);
	
	// save GraphBuffer - to restore it later	
	save_restoreGB(1);

	// first get high and low values
	for (i = 0; i < GraphTraceLen; i++) {
		if (GraphBuffer[i] > high)
			high = GraphBuffer[i];
		else if (GraphBuffer[i] < low)
			low = GraphBuffer[i];
	}

	i = 0;
	j = 0;
	minClk = 255;
	// get to first full low to prime loop and skip incomplete first pulse
	while ((GraphBuffer[i] < high) && (i < GraphTraceLen))
		++i;
	while ((GraphBuffer[i] > low) && (i < GraphTraceLen))
		++i;
	skip = i;

	// populate tmpbuff buffer with pulse lengths
	while (i < GraphTraceLen) {
		// measure from low to low
		while ((GraphBuffer[i] > low) && (i < GraphTraceLen))
			++i;
		start= i;
		while ((GraphBuffer[i] < high) && (i < GraphTraceLen))
			++i;
		while ((GraphBuffer[i] > low) && (i < GraphTraceLen))
			++i;
		if (j>=(MAX_GRAPH_TRACE_LEN/64)) {
			break;
		}
		tmpbuff[j++]= i - start;
		if (i-start < minClk && i < GraphTraceLen) {
			minClk = i - start;
		}
	}
	// set clock
	if (!clk) {
		for (uint8_t clkCnt = 0; clkCnt<7; clkCnt++) {
			tol = fndClk[clkCnt]/8;
			if (minClk >= fndClk[clkCnt]-tol && minClk <= fndClk[clkCnt]+1) { 
				clk=fndClk[clkCnt];
				break;
			}
		}
		if (!clk) return 0;
	} else tol = clk/8;

	// look for data start - should be 2 pairs of LW (pulses of clk*3,clk*2)
	start = -1;
	for (i= 0; i < j - 4 ; ++i) {
		skip += tmpbuff[i];
		if (tmpbuff[i] >= clk*3-tol && tmpbuff[i] <= clk*3+tol)  //3 clocks
			if (tmpbuff[i+1] >= clk*2-tol && tmpbuff[i+1] <= clk*2+tol)  //2 clocks
				if (tmpbuff[i+2] >= clk*3-tol && tmpbuff[i+2] <= clk*3+tol) //3 clocks
					if (tmpbuff[i+3] >= clk-tol)  //1.5 to 2 clocks - depends on bit following
					{
						start= i + 4;
						break;
					}
	}
	startblock = i + 4;

	// skip over the remainder of LW
	skip += tmpbuff[i+1] + tmpbuff[i+2] + clk;
	if (tmpbuff[i+3]>clk) 
		phaseoff = tmpbuff[i+3]-clk;
	else
		phaseoff = 0;
	// now do it again to find the end
	end = skip;
	for (i += 3; i < j - 4 ; ++i) {
		end += tmpbuff[i];
		if (tmpbuff[i] >= clk*3-tol && tmpbuff[i] <= clk*3+tol)  //3 clocks
			if (tmpbuff[i+1] >= clk*2-tol && tmpbuff[i+1] <= clk*2+tol)  //2 clocks
				if (tmpbuff[i+2] >= clk*3-tol && tmpbuff[i+2] <= clk*3+tol) //3 clocks
					if (tmpbuff[i+3] >= clk-tol)  //1.5 to 2 clocks - depends on bit following
					{
						complete= true;
						break;
					}
	}
	end = i;
	// report back
	if (verbose || g_debugMode) {
		if (start >= 0) {
			PrintAndLog("\nNote: one block = 50 bits (32 data, 12 parity, 6 marker)");
		}	else {
			PrintAndLog("No data found!, clock tried:%d",clk);
			PrintAndLog("Try again with more samples.");
			PrintAndLog("  or after a 'data askedge' command to clean up the read");
			return 0;
		}
	} else if (start < 0) return 0;
	start = skip;
	snprintf(tmp2, sizeof(tmp2),"%d %d 1000 %d", clk, invert, clk*47);
	// get rid of leading crap 
	snprintf(tmp, sizeof(tmp), "%i", skip);
	CmdLtrim(tmp);
	bool pTest;
	bool AllPTest = true;
	// now work through remaining buffer printing out data blocks
	block = 0;
	i = startblock;
	while (block < 6) {
		if (verbose || g_debugMode) PrintAndLog("\nBlock %i:", block);
		skip = phaseoff;
		
		// look for LW before start of next block
		for ( ; i < j - 4 ; ++i) {
			skip += tmpbuff[i];
			if (tmpbuff[i] >= clk*3-tol && tmpbuff[i] <= clk*3+tol)
				if (tmpbuff[i+1] >= clk-tol)
					break;
		}
		if (i >= j-4) break; //next LW not found
		skip += clk;
		if (tmpbuff[i+1]>clk)
			phaseoff = tmpbuff[i+1]-clk;
		else
			phaseoff = 0;
		i += 2;
		if (ASKDemod(tmp2, false, false, 1) < 1) {
			save_restoreGB(0);
			return 0;
		}
		//set DemodBufferLen to just one block
		DemodBufferLen = skip/clk;
		//test parities
		pTest = EM_ByteParityTest(DemodBuffer,DemodBufferLen,5,9,0);	
		pTest &= EM_EndParityTest(DemodBuffer,DemodBufferLen,5,9,0);
		AllPTest &= pTest;
		//get output
		Code[block] = OutputEM4x50_Block(DemodBuffer,DemodBufferLen,verbose, pTest);
		if (g_debugMode) PrintAndLog("\nskipping %d samples, bits:%d", skip, skip/clk);
		//skip to start of next block
		snprintf(tmp,sizeof(tmp),"%i",skip);
		CmdLtrim(tmp);
		block++;
		if (i >= end) break; //in case chip doesn't output 6 blocks
	}
	//print full code:
	if (verbose || g_debugMode || AllPTest){
		if (!complete) {
			PrintAndLog("*** Warning!");
			PrintAndLog("Partial data - no end found!");
			PrintAndLog("Try again with more samples.");
		}
		PrintAndLog("Found data at sample: %i - using clock: %i", start, clk);    
		end = block;
		for (block=0; block < end; block++){
			PrintAndLog("Block %d: %08x",block,Code[block]);
		}
		if (AllPTest) {
			PrintAndLog("Parities Passed");
		} else {
			PrintAndLog("Parities Failed");
			PrintAndLog("Try cleaning the read samples with 'data askedge'");
		}
	}

	//restore GraphBuffer
	save_restoreGB(0);
	return (int)AllPTest;
}

int CmdEM4x50Read(const char *Cmd)
{
	return EM4x50Read(Cmd, true);
}

//**************** Start of EM4x05/EM4x69 Code ************************
int usage_lf_em_read(void) {
	PrintAndLog("Read EM4x05/EM4x69.  Tag must be on antenna. ");
	PrintAndLog("");
	PrintAndLog("Usage:  lf em 4x05readword [h] <address> <pwd>");
	PrintAndLog("Options:");
	PrintAndLog("       h         - this help");
	PrintAndLog("       address   - memory address to read. (0-15)");
	PrintAndLog("       pwd       - password (hex) (optional)");
	PrintAndLog("samples:");
	PrintAndLog("      lf em 4x05readword 1");
	PrintAndLog("      lf em 4x05readword 1 11223344");
	return 0;
}

// for command responses from em4x05 or em4x69
// download samples from device and copy them to the Graphbuffer
bool downloadSamplesEM() {
	// 8 bit preamble + 32 bit word response (max clock (128) * 40bits = 5120 samples)
	uint8_t got[6000]; 
	GetFromBigBuf(got, sizeof(got), 0);
	if ( !WaitForResponseTimeout(CMD_ACK, NULL, 4000) ) {
		PrintAndLog("command execution time out");
		return false;
	}
	setGraphBuf(got, sizeof(got));
	return true;
}

bool EM4x05testDemodReadData(uint32_t *word, bool readCmd) {
	// em4x05/em4x69 preamble is 00001010
	// skip first two 0 bits as they might have been missed in the demod
	uint8_t preamble[] = {0,0,1,0,1,0};
	size_t startIdx = 0;
	// set size to 20 to only test first 14 positions for the preamble
	size_t size = (20 > DemodBufferLen) ? DemodBufferLen : 20;

	//test preamble
	if ( !onePreambleSearch(DemodBuffer, preamble, sizeof(preamble), size, &startIdx) ) {
		if (g_debugMode) PrintAndLog("DEBUG: Error - EM4305 preamble not found :: %d", startIdx);
		return false;
	}
	// if this is a readword command, get the read bytes and test the parities
	if (readCmd) {
		if (!EM_EndParityTest(DemodBuffer + startIdx + sizeof(preamble), 45, 5, 9, 0)) {
			if (g_debugMode) PrintAndLog("DEBUG: Error - End Parity check failed");
			return false;
		}
		//test for even parity bits.
		if ( removeParity(DemodBuffer, startIdx + sizeof(preamble),9,0,44) == 0 ) {		
			if (g_debugMode) PrintAndLog("DEBUG: Error - Parity not detected");
			return false;
		}

		setDemodBuf(DemodBuffer, 40, 0);
		*word = bytebits_to_byteLSBF(DemodBuffer, 32);
	}
	return true;
}

// FSK, PSK, ASK/MANCHESTER, ASK/BIPHASE, ASK/DIPHASE 
// should cover 90% of known used configs
// the rest will need to be manually demoded for now...
int demodEM4x05resp(uint32_t *word, bool readCmd) {
	int ans = 0;

	// test for FSK wave (easiest to 99% ID)
	if (GetFskClock("", false, false)) {
		//valid fsk clocks found
		ans = FSKrawDemod("0 0", false);
		if (!ans) {
			if (g_debugMode) PrintAndLog("DEBUG: Error - EM4305: FSK Demod failed, ans: %d", ans);
		} else {
			if (EM4x05testDemodReadData(word, readCmd)) {
				return 1;
			}
		}
	}
	// PSK clocks should be easy to detect ( but difficult to demod a non-repeating pattern... )
	ans = GetPskClock("", false, false);
	if (ans>0) {
		//try psk1
		ans = PSKDemod("0 0 6", false);
		if (!ans) {
			if (g_debugMode) PrintAndLog("DEBUG: Error - EM4305: PSK1 Demod failed, ans: %d", ans);
		} else {
			if (EM4x05testDemodReadData(word, readCmd)) {
				return 1;
			} else {
				//try psk2
				psk1TOpsk2(DemodBuffer, DemodBufferLen);
				if (EM4x05testDemodReadData(word, readCmd)) {
					return 1;
				}
			}
			//try psk1 inverted
			ans = PSKDemod("0 1 6", false);
			if (!ans) {
				if (g_debugMode) PrintAndLog("DEBUG: Error - EM4305: PSK1 Demod failed, ans: %d", ans);
			} else {
				if (EM4x05testDemodReadData(word, readCmd)) {
					return 1;
				} else {
					//try psk2
					psk1TOpsk2(DemodBuffer, DemodBufferLen);
					if (EM4x05testDemodReadData(word, readCmd)) {
						return 1;
					}
				}
			}
		}
	}

	// manchester is more common than biphase... try first
	bool stcheck = false;
	// try manchester - NOTE: ST only applies to T55x7 tags.
	ans = ASKDemod_ext("0,0,1", false, false, 1, &stcheck);
	if (!ans) {
		if (g_debugMode) PrintAndLog("DEBUG: Error - EM4305: ASK/Manchester Demod failed, ans: %d", ans);
	} else {
		if (EM4x05testDemodReadData(word, readCmd)) {
			return 1;
		}
	}

	//try biphase
	ans = ASKbiphaseDemod("0 0 1", false);
	if (!ans) { 
		if (g_debugMode) PrintAndLog("DEBUG: Error - EM4305: ASK/biphase Demod failed, ans: %d", ans);
	} else {
		if (EM4x05testDemodReadData(word, readCmd)) {
			return 1;
		}
	}

	//try diphase (differential biphase or inverted)
	ans = ASKbiphaseDemod("0 1 1", false);
	if (!ans) { 
		if (g_debugMode) PrintAndLog("DEBUG: Error - EM4305: ASK/biphase Demod failed, ans: %d", ans);
	} else {
		if (EM4x05testDemodReadData(word, readCmd)) {
			return 1;
		}
	}

	return -1;
}

int EM4x05ReadWord_ext(uint8_t addr, uint32_t pwd, bool usePwd, uint32_t *wordData) {
	UsbCommand c = {CMD_EM4X_READ_WORD, {addr, pwd, usePwd}};
	clearCommandBuffer();
	SendCommand(&c);
	UsbCommand resp;	
	if (!WaitForResponseTimeout(CMD_ACK, &resp, 2500)){
		PrintAndLog("Command timed out");
		return -1;
	}
	if ( !downloadSamplesEM() ) {
		return -1;
	}
	int testLen = (GraphTraceLen < 1000) ? GraphTraceLen : 1000;
	if (graphJustNoise(GraphBuffer, testLen)) {
		PrintAndLog("no tag not found");
		return -1;
	}
	//attempt demod:
	return demodEM4x05resp(wordData, true);
}

int EM4x05ReadWord(uint8_t addr, uint32_t pwd, bool usePwd) {
	uint32_t wordData = 0;
	int success = EM4x05ReadWord_ext(addr, pwd, usePwd, &wordData);
	if (success == 1)
		PrintAndLog(" Got Address %02d | %08X",addr,wordData);
	else
		PrintAndLog("Read Address %02d | failed",addr);

	return success;
}

int CmdEM4x05ReadWord(const char *Cmd) {
	uint8_t addr;
	uint32_t pwd;
	bool usePwd = false;
	uint8_t ctmp = param_getchar(Cmd, 0);
	if ( strlen(Cmd) == 0 || ctmp == 'H' || ctmp == 'h' ) return usage_lf_em_read();

	addr = param_get8ex(Cmd, 0, 50, 10);
	// for now use default input of 1 as invalid (unlikely 1 will be a valid password...)
	pwd =  param_get32ex(Cmd, 1, 1, 16);
	
	if ( (addr > 15) ) {
		PrintAndLog("Address must be between 0 and 15");
		return 1;
	}
	if ( pwd == 1 ) {
		PrintAndLog("Reading address %02u", addr);
	}	else {
		usePwd = true;
		PrintAndLog("Reading address %02u | password %08X", addr, pwd);
	}

	return EM4x05ReadWord(addr, pwd, usePwd);
}

int usage_lf_em_dump(void) {
	PrintAndLog("Dump EM4x05/EM4x69.  Tag must be on antenna. ");
	PrintAndLog("");
	PrintAndLog("Usage:  lf em 4x05dump [h] <pwd>");
	PrintAndLog("Options:");
	PrintAndLog("       h         - this help");
	PrintAndLog("       pwd       - password (hex) (optional)");
	PrintAndLog("samples:");
	PrintAndLog("      lf em 4x05dump");
	PrintAndLog("      lf em 4x05dump 11223344");
	return 0;
}

int CmdEM4x05dump(const char *Cmd) {
	uint8_t addr = 0;
	uint32_t pwd;
	bool usePwd = false;
	uint8_t ctmp = param_getchar(Cmd, 0);
	if ( ctmp == 'H' || ctmp == 'h' ) return usage_lf_em_dump();

	// for now use default input of 1 as invalid (unlikely 1 will be a valid password...)
	pwd = param_get32ex(Cmd, 0, 1, 16);
	
	if ( pwd != 1 ) {
		usePwd = true;
	}
	int success = 1;
	for (; addr < 16; addr++) {
		if (addr == 2) {
			if (usePwd) {
				PrintAndLog(" PWD Address %02u | %08X",addr,pwd);
			} else {
				PrintAndLog(" PWD Address 02 | cannot read");
			}
		} else {
			success &= EM4x05ReadWord(addr, pwd, usePwd);
		}
	}

	return success;
}


int usage_lf_em_write(void) {
	PrintAndLog("Write EM4x05/EM4x69.  Tag must be on antenna. ");
	PrintAndLog("");
	PrintAndLog("Usage:  lf em 4x05writeword [h] <address> <data> <pwd>");
	PrintAndLog("Options:");
	PrintAndLog("       h         - this help");
	PrintAndLog("       address   - memory address to write to. (0-15)");
	PrintAndLog("       data      - data to write (hex)");	
	PrintAndLog("       pwd       - password (hex) (optional)");
	PrintAndLog("samples:");
	PrintAndLog("      lf em 4x05writeword 1");
	PrintAndLog("      lf em 4x05writeword 1 deadc0de 11223344");
	return 0;
}

int CmdEM4x05WriteWord(const char *Cmd) {
	uint8_t ctmp = param_getchar(Cmd, 0);
	if ( strlen(Cmd) == 0 || ctmp == 'H' || ctmp == 'h' ) return usage_lf_em_write();
	
	bool usePwd = false;
		
	uint8_t addr = 16; // default to invalid address
	uint32_t data = 0xFFFFFFFF; // default to blank data
	uint32_t pwd = 0xFFFFFFFF; // default to blank password
	
	addr = param_get8ex(Cmd, 0, 16, 10);
	data = param_get32ex(Cmd, 1, 0, 16);
	pwd =  param_get32ex(Cmd, 2, 1, 16);
	
	
	if ( (addr > 15) ) {
		PrintAndLog("Address must be between 0 and 15");
		return 1;
	}
	if ( pwd == 1 )
		PrintAndLog("Writing address %d data %08X", addr, data);	
	else {
		usePwd = true;
		PrintAndLog("Writing address %d data %08X using password %08X", addr, data, pwd);		
	}
	
	uint16_t flag = (addr << 8 ) | usePwd;
	
	UsbCommand c = {CMD_EM4X_WRITE_WORD, {flag, data, pwd}};
	clearCommandBuffer();
	SendCommand(&c);
	UsbCommand resp;	
	if (!WaitForResponseTimeout(CMD_ACK, &resp, 2000)){
		PrintAndLog("Error occurred, device did not respond during write operation.");
		return -1;
	}
	if ( !downloadSamplesEM() ) {
		return -1;
	}
	//check response for 00001010 for write confirmation!	
	//attempt demod:
	uint32_t dummy = 0;
	int result = demodEM4x05resp(&dummy,false);
	if (result == 1) {
		PrintAndLog("Write Verified");
	}
	return result;
}

void printEM4x05info(uint8_t chipType, uint8_t cap, uint16_t custCode, uint32_t serial) {
	switch (chipType) {
		case 9: PrintAndLog("\nChip Type:   %u | EM4305", chipType); break;
		case 4: PrintAndLog("Chip Type:   %u | Unknown", chipType); break;
		case 2: PrintAndLog("Chip Type:   %u | EM4469", chipType); break;
		//add more here when known
		default: PrintAndLog("Chip Type:   %u Unknown", chipType); break;
	}

	switch (cap) {
		case 3: PrintAndLog(" Cap Type:   %u | 330pF",cap); break;
		case 2: PrintAndLog(" Cap Type:   %u | %spF",cap, (chipType==2)? "75":"210"); break;
		case 1: PrintAndLog(" Cap Type:   %u | 250pF",cap); break;
		case 0: PrintAndLog(" Cap Type:   %u | no resonant capacitor",cap); break;
		default: PrintAndLog(" Cap Type:   %u | unknown",cap); break;
	}

	PrintAndLog("Cust Code: %03u | %s", custCode, (custCode == 0x200) ? "Default": "Unknown");
	if (serial != 0) {
		PrintAndLog("\n Serial #: %08X\n", serial);
	}
}

//quick test for EM4x05/EM4x69 tag
bool EM4x05Block0Test(uint32_t *wordData) {
	if (EM4x05ReadWord_ext(0,0,false,wordData) == 1) {
		return true;
	}
	return false;
}

int CmdEM4x05info(const char *Cmd) {
	//uint8_t addr = 0;
	//uint32_t pwd;
	uint32_t wordData = 0;
  //	bool usePwd = false;
	uint8_t ctmp = param_getchar(Cmd, 0);
	if ( ctmp == 'H' || ctmp == 'h' ) return usage_lf_em_dump();

	// for now use default input of 1 as invalid (unlikely 1 will be a valid password...)
	//pwd = param_get32ex(Cmd, 0, 1, 16);
	
	//if ( pwd != 1 ) {
	//	usePwd = true;
	//}
	int success = 1;
	// read blk 0

	//block 0 can be read even without a password.
	if ( !EM4x05Block0Test(&wordData) ) 
		return -1;
	
	uint8_t chipType = (wordData >> 1) & 0xF;
	uint8_t cap = (wordData >> 5) & 3;
	uint16_t custCode = (wordData >> 9) & 0x3FF;
	
	wordData = 0;
	if (EM4x05ReadWord_ext(1, 0, false, &wordData) != 1) {
		//failed, but continue anyway...
	}
	printEM4x05info(chipType, cap, custCode, wordData);

	// add read block 4 and read out config if successful
	// needs password if one is set

	return success;
}


static command_t CommandTable[] =
{
	{"help", CmdHelp, 1, "This help"},
	{"410xdemod", CmdEMdemodASK, 0, "[findone] -- Extract ID from EM410x tag (option 0 for continuous loop, 1 for only 1 tag)"},  
	{"410xread", CmdEM410xRead, 1, "[clock rate] -- Extract ID from EM410x tag in GraphBuffer"},
	{"410xsim", CmdEM410xSim, 0, "<UID> [clock rate] -- Simulate EM410x tag"},
	{"410xwatch", CmdEM410xWatch, 0, "['h'] -- Watches for EM410x 125/134 kHz tags (option 'h' for 134)"},
	{"410xspoof", CmdEM410xWatchnSpoof, 0, "['h'] --- Watches for EM410x 125/134 kHz tags, and replays them. (option 'h' for 134)" },
	{"410xwrite", CmdEM410xWrite, 0, "<UID> <'0' T5555> <'1' T55x7> [clock rate] -- Write EM410x UID to T5555(Q5) or T55x7 tag, optionally setting clock rate"},
	{"4x05dump", CmdEM4x05dump, 0, "(pwd) -- Read EM4x05/EM4x69 all word data"},
	{"4x05info", CmdEM4x05info, 0, "(pwd) -- Get info from EM4x05/EM4x69 tag"},
	{"4x05readword", CmdEM4x05ReadWord, 0, "<Word> (pwd) -- Read EM4x05/EM4x69 word data"},
	{"4x05writeword", CmdEM4x05WriteWord, 0, "<Word> <data> (pwd) -- Write EM4x05/EM4x69 word data"},
	{"4x50read", CmdEM4x50Read, 1, "demod data from EM4x50 tag from the graph buffer"},
	{NULL, NULL, 0, NULL}
};

int CmdLFEM4X(const char *Cmd)
{
	CmdsParse(CommandTable, Cmd);
	return 0;
}

int CmdHelp(const char *Cmd)
{
	CmdsHelp(CommandTable);
	return 0;
}
