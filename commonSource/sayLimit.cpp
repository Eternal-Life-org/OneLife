#include <math.h>

int getSayLimit( double inAge ) {
    int floorAge = floor( inAge );
    
    int sayCap = (int)( floorAge + 1 );
    
    int adultBase = 50;

    if( floorAge >= 16 ) {
        sayCap = 16 + ( floorAge - 16 ) / 2 + adultBase;
        }
    else if ( floorAge >= 8 ) {
        // rise smoothly from 9 to ( 16 + adultBase) characters between
        // age 8 and age 16
        
        int extraAge = floorAge - 8;
        
        if( extraAge > 0 ) {
            int sixteenLimit = adultBase + 16;
            
            int fullIncrease = sixteenLimit - 9;
            
            double fraction = extraAge / 8.0;

            // sigmoid curve with 0,1 output for input values between 0 and 1
            double hardness = 12;
            double curvedFraction = 
                1.0 / ( 1.0 + pow( 2.0, - hardness *( fraction - 0.5 ) ) );
            
            double increase = fullIncrease * curvedFraction;
            
            sayCap = 9 + floor( increase );
            }
        }

    return sayCap;
    }



int truncateToUTF8CharCount( char *s, int maxChars ) {
    int i = 0;
    int numChars = 0;

    while( s[i] != '\0' ) {
        if( numChars == maxChars ) {
            s[i] = '\0';
            return numChars;
            }
        unsigned char c = (unsigned char)s[i];
        int charBytes;
        if( ( c & 0x80 ) == 0 ) {
            charBytes = 1;
            }
        else if( ( c & 0xE0 ) == 0xC0 ) {
            charBytes = 2;
            }
        else if( ( c & 0xF0 ) == 0xE0 ) {
            charBytes = 3;
            }
        else if( ( c & 0xF8 ) == 0xF0 ) {
            charBytes = 4;
            }
        else {
            // 非法/孤立的 UTF-8 后续字节,按单字节处理
            charBytes = 1;
            }
        numChars++;
        // 安全前进一个字符:声明的字节数可能超过字符串实际剩余,
        // 遇 \0 即停,避免越界
        for( int b = 0; b < charBytes && s[i] != '\0'; b++ ) {
            i++;
            }
        }
    return numChars;
    }

