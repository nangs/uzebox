/*
 *  Uzebox(tm) mconvert utility
 *  2017 Lee Weber
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  Uzebox is a reserved trade mark
*/

/* This tool converts standard midiconv output into a compressed version
 * as C array or binary(can be placed in existing resource file) for use
 * by the streaming music player. It is intended to improve efficiency
 * over the old MIDI format.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>

FILE *fin, *fout;

int asBin;
int asPipe;
int doPad;
int doDebug;
int doCustomName;
char arrayName[64];
long ctrFilter;

long outOff;
long runTime;
int inSize,outSize;
unsigned char inBuf[1024*64];
unsigned char outBuf[1024*64];

long loopStart;
unsigned char loopBuf[256];

#define FILTER_CHANNEL_VOLUME	1
#define FILTER_EXPRESSION	2
#define FILTER_TREMOLO_VOLUME	4
#define FILTER_TREMOLO_RATE	8
#define FILTER_NOTE_OFF		16

int ConvertAndWrite();

int main(int argc, char *argv[]){

	int i;
		
	if(argc < 1+2){
		printf("mconvert - compressed MIDI convertor for Uzebox\n");
		printf("\tconverts a C array file generated by midiconv, and outputs\n");
		printf("\ta compressed version for the streaming music player\n");
		printf("\t\tusage: mconvert midi_input.mid compressed_out.inc -flags\n");
		printf("\toptional flags:\n");
		printf("\t\t\t-p : piped output, write to stdout instead of a file(no stats)\n");
		printf("\t\t\t-b : outputs binary instead of a text C style array\n");
		printf("\t\t\t-a : pads the output to fill to a 512 byte sector boundary\n");
		printf("\t\t\t-o : output to a specific offset in the file(binary mode)\n");
		printf("\t\t\t-n : specify the output array name(text mode only)\n");
		printf("\t\t\t-d : output in debug mode(C array of binary values)\n");
		printf("\t\t\t-f : apply a (bit flag)filter on controller events:\n");
		printf("\t\t\t\t1 : remove Channel Volume controllers\n");
		printf("\t\t\t\t2 : remove Expression controllers\n");
		printf("\t\t\t\t4 : remove Tremolo Volume controllers\n");
		printf("\t\t\t\t8 : remove Tremolo Rate controllers\n");
		printf("\t\t\t\t16 : remove note off events(volume 0)\n");
		printf("\t\t\t\tie. : \"-f 1 removes Channel Volume,\n");
		printf("\t\t\t\t\"-f 15\" or \"-f 255\" removes all\n");
		goto DONE;
	}

	strcpy(arrayName,"CompressedSong");/* set a default output array name */

	for(i=1;i<argc;i++){ /* handle command line arguments */
		if(!strcmp(argv[i],"-b"))/* output in binary mode */
			asBin = 1;
		else if(!strcmp(argv[i],"-a"))/* padded output mode */
			doPad = 1;
		else if(!strcmp(argv[i],"-d"))/* debug output mode */
			doDebug = 1;
		else if(!strcmp(argv[i],"-o")){/* explicit output offset */
			if(i == argc-1){
				printf("Error: no offset specified for -o flag\n");
				goto ERROR;
			}
			outOff = strtol(argv[i+1],NULL,0);
			i++;
		}else if(!strcmp(argv[i],"-n")){/* custom array name */
			if(i == argc-1){
				printf("Error: no name specified for -n flag\n");
				goto ERROR;
			}
			if(strlen(argv[i+1]) >= sizeof(arrayName)){
				printf("Error: array name is too long\n");
				goto ERROR;
			}
			strcpy(arrayName,argv[i+1]);
			doCustomName = 1;
			i++;
		}else if(!strcmp(argv[i],"-f")){/* controller filters */
			if(i == argc-1){
				printf("Error: no argument supplied for controller filter\n");
				goto ERROR;
			}
			ctrFilter = strtol(argv[i+1],NULL,0);
			i++;
		}
	}
	
	fin = fopen(argv[1],"r");
	if(fin == NULL){
		printf("Error: Failed to open input file: %s\n",argv[1]);		
		goto ERROR;
	}

	fout = fopen(argv[2],asBin ? "rb+":"w");/* non-binary destroys any previous C array output */
	if(fout == NULL){ /* file does not exist, try to create it. */
		printf("Binary output file does not exist\n");		
		if(asBin){
			fout = fopen(argv[2],"wb");
			if(fout == NULL){
				printf("Error: Failed to create output file: %s\n",argv[2]);
				goto ERROR;
			}
			printf("Created new binary file\n");
			if(outOff)
				printf("Padding new binary file with %ld '0's\n",outOff);
			for(i=0;i<outOff;i++)/* pad the new file with 0 up to the specified offset */
				fputc(0,fout);
		}else{/* the "w" should have succeeded */
			printf("Error: Failed to create output file: %s\n",argv[2]);		
			goto ERROR;
		}
	}else if(asBin){/* only binary mode supports output offset, text mode is always a new empty file */
		printf("Opened binary file for output at offset: %ld\n",outOff);	
		fseek(fout,outOff,SEEK_SET);
	}else/* creation of text file succeeded */
		printf("Created text file \"%s\" for output\n",argv[2]);
	
	i = ConvertAndWrite();
	
	if(i != 1){
		printf("Error: conversion failed: %d\n",i);
		goto ERROR;
	}


	goto DONE;
ERROR:
	exit(0);
DONE:	
	exit(1);
}



int ConvertAndWrite(){
	int foundSongEnd,delta,i,j,w;
	unsigned char c1,c2,c3,t,lastStatus,channel;
w = 0;
	loopStart = -1;

	while(fgetc(fin) != '{' && !feof(fin));/* eat everything up to the beginning of the array data */

	if(feof(fin))/* got the the end of the file without seeing the opening bracket of the array */
		return -1;

	while(fscanf(fin," 0%*[xX]%x , ",&i) == 1)
		inBuf[inSize++] = (i & 0xFF);

	i = 1; /* skip first delta */

	lastStatus = 0;
	while(i < inSize){
		if(i > sizeof(outBuf)-10){
			printf("Error: outBuf out of bounds\n");
			return -2;
		}

		c1 = inBuf[i++];
		
		if(c1 == 0xFF){/* a meta event */
			c1 = inBuf[i++];
			if(c1 == 0x2F){/* end of song */
				outBuf[outSize++] = 		0b11000010;/* Song End */
				foundSongEnd = 1;
				break;

			}else if(c1 == 0x06){ /* Loop markers or unsupported command */
				c1 = inBuf[i++]; /* eat len byte */
				c2 = inBuf[i++]; /* get data */				
				if(c2 == 'S'){
					outBuf[outSize++] = 	0b11000001;/* Loop Start */
					loopStart = outSize;				
				}else if(c2 == 'E'){
					outBuf[outSize++] = 	0b11000000;/* Loop End */
					foundSongEnd = 1;
					break;				
				}else{/* something that midiconv should not have output */
					printf("Error: Got an unrecognized command\n");				
					return -4;
				}			
			}

		}else{
			if(c1 & 0x80)
				lastStatus = c1;			
			channel = (lastStatus & 0x0F);
			if(channel > 4){
				printf("\nError: Got bad channel:%d from byte:%d at offset %d\n",channel,c1,outSize);
				return -5;
			}

			if(c1 & 0x80)
				c1 = inBuf[i++];

			switch(lastStatus & 0xF0){
				
				case 0x90: /* Note On event, c1 = note, 2 bytes */
					/* 0b1CCCVVVV, 0bVNNNNNNN */
					c2 = inBuf[i++];/* c2 = 7 bit volume */
					c2 >>= 1; /* convert to 6 bit volume used in the compressed format */
					if(c2 || !(ctrFilter & FILTER_NOTE_OFF)){/* Note Off is a Note On with volume 0 */
						outBuf[outSize++] = (channel<<5) | (c2 & 0b00011111);/* 0bCCCVVVVV */
						outBuf[outSize++] = ((c2 & 0b00100000)<<2) | (c1 & 0b01111111);/* 0bVNNNNNNN MSBit of volume*/
					}					
					break;

				case 0xB0:/* controller, c1 = type, 2 bytes */
					c2 = inBuf[i++];/* c2 = controller value */
					/* 0b111XXCCC */
					if(c1 == 0x07){/* channel volume */
						if(!(ctrFilter & FILTER_CHANNEL_VOLUME))
							outBuf[outSize++] = 0b11100000 | channel;
						else
							break;/* don't write controller value */
					}else if(c1 == 0x0B){ /* expression */
						if(!(ctrFilter & FILTER_EXPRESSION))
							outBuf[outSize++] = 0b11101000 | channel;
						else
							break;										
					}else if(c1 == 0x5C){ /* tremolo volume */
						if(!(ctrFilter & FILTER_TREMOLO_VOLUME))
							outBuf[outSize++] = 0b11110000 | channel;
						else
							break;					
					}else if(c1 == 0x64){ /* tremolo rate */
						if(!(ctrFilter & FILTER_TREMOLO_RATE))
							outBuf[outSize++] = 0b11111000 | channel;
						else
							break;					
					}else{/* got something that midiconv should not have output */
						printf("Error: Got unknown controller event\n");					
						return -6;
					}
					
					outBuf[outSize++] = c2;/* controller value */
					break;

				case 0xC0:/* program change, c1 = patch */
						outBuf[outSize++] = 0b10100000 | channel;/*0b10100CCC*/
						outBuf[outSize++] = c1; /* patch */
					break;
				default:
					printf("Error: Got unknown controller\n");
					return -1;					
			}/* end switch(lastStatus & 0x0F) */
		}/* end else(c1 != 0xFF) */

		delta = inBuf[i++]; /* calculate the next delta time, which is possibly encoded as multiple bytes */

		if(delta & 0x80){
			delta &= 0x7F;
			do{
				c3 = inBuf[i++];
				delta = (delta<<7) + (c3 & 0x7F);
			}while(c3 & 0x80);
		}

		if(delta == 0)/* we do not store deltas of 0(it is implied in the format) */
			continue;

		runTime += delta;/* keep track of how long this song is, in 60hz ticks, for statistic display */
			
		while(delta){			
			/* for a non-zero delta, we create a Tick End event */
			if(delta < 8){/* store 1 to 7 frame delays in the same byte as the command, 0 = 1, 1 = 2, etc. */
				outBuf[outSize++] = 0b11000011 | (((delta-1) & 0b00000111)<<2);/* 0b110DDD11, stored as 1 less than actual delay */
				delta = 0;
			}else{ /* we store longer delays in a 2 byte format */			
				outBuf[outSize++] = 0b11011111;/* delta of 7(+1) indicates that the next byte holds the actual delay */				
				if(delta > 254){/* we can store any delay as a series of 8 bit values */
					outBuf[outSize++] = 0b11111111;
					delta -= 254;
				}else{
					outBuf[outSize++] = (delta & 0b11111111);
					delta = 0;
				}
			}
		}
 

	
	}

	if(i >= inSize)/* the data left some command incomplete, ie. extra or not enough bytes */
		return -3;

	if(!foundSongEnd){
		printf("Error: Did not find song or loop end point\n");
		return -2;
	}

	if(loopStart >= 0)	
		for(i=0;i<sizeof(loopBuf);i++)
			outBuf[outSize++] = outBuf[i+loopStart];

	if(doPad)/* pad out the size to fill the full sector */
		while(outSize%512)
			outBuf[outSize++] = 0xFF;

	if(!asPipe){
		if(asBin){
			for(i=0;i<outSize;i++)/* output the actual data, text or binary, and any offset were already setup prior */
				fputc(outBuf[i],fout);
		}else{/* C array */
			fprintf(fout,"const char %s[] PROGMEM = {\n",arrayName);
			w = 0;
			for(i=0;i<outSize;i++){
				if(!doDebug)
					fprintf(fout,"0x%02X,",outBuf[i]);
				else{
					fprintf(fout,"0b");
					for(j=0;j<8;j++)
						fputc(((outBuf[i]<<j)&128) ? '1':'0',fout);
					fputc(',',fout);
					w = 100;			
				}	
				if(++w > 15){
					w = 0;
					fprintf(fout,"\n");
				}			
			}
			fprintf(fout,"\n};\n");
		}
		/* display statistics unless we are in pipe mode */
		printf("\n\tRun time: %ld seconds\n",(runTime/60));
		printf("\tInput data size: %d\n",inSize);
		if(loopStart == -1)
			printf("\tOutput data size: %d\n",outSize);
		else
			printf("\tOutput data size: %d(+%d loop)\n",outSize-sizeof(loopBuf),sizeof(loopBuf));	
			printf("\tAverage bytes per frame: %ld\n",(outSize/runTime));
			printf("\tAverage bytes per second: %ld\n",(outSize/(runTime/60)));		
	
	}else{/* output the data directly to stdout, for piping */
		for(i=0;i<outSize;i++)
			putchar(outBuf[i]);
	}

	return 1;
}
