#include "TextField.h"
#include "DropdownList.h"

#include <string.h>

#include "minorGems/game/game.h"
#include "minorGems/game/gameGraphics.h"
#include "minorGems/game/drawUtils.h"
#include "minorGems/util/stringUtils.h"
#include "minorGems/graphics/openGL/KeyboardHandlerGL.h"

#include <string>



// start:  none focused
TextField *TextField::sFocusedTextField = NULL;

extern double frameRateFactor;

int TextField::sDeleteFirstDelaySteps = 30 / frameRateFactor;
int TextField::sDeleteNextDelaySteps = 2 / frameRateFactor;

// shortcuts off by default (game fields don't use them)
// the editor turns this on for all of its fields
char TextField::sPasteShortcutForNewFields = false;


// snaps a byte index to the start of the UTF-8 character containing it
// (UTF-8 continuation bytes match 10xxxxxx)
static int utf8CharStart( const char *inText, int inPos ) {
    while( inPos > 0 && ( (unsigned char)inText[ inPos ] & 0xC0 ) == 0x80 ) {
        inPos--;
        }
    return inPos;
    }




TextField::TextField( Font *inDisplayFont, 
                      double inX, double inY, int inCharsWide,
                      char inForceCaps,
                      const char *inLabelText,
                      const char *inAllowedChars,
                      const char *inForbiddenChars,
                      char inDrawLabelWithShadow )
        : PageComponent( inX, inY ),
          mActive( true ),
          mContentsHidden( false ),
          mHiddenSprite( loadSprite( "hiddenFieldTexture.tga", false ) ),
          mFont( inDisplayFont ), 
          mCharsWide( inCharsWide ),
          mMaxLength( -1 ),
          mFireOnAnyChange( false ),
          mFireOnLeave( false ),
          mForceCaps( inForceCaps ),
          mLabelText( NULL ),
          mAllowedChars( NULL ), mForbiddenChars( NULL ),
          mHover( false ),
          mFocused( false ), mText( new char[1] ),
          mTextLen( 0 ),
          mCursorPosition( 0 ),
          mIgnoreArrowKeys( false ),
          mIgnoreMouse( false ),
          mDrawnText( NULL ),
          mCursorDrawPosition( 0 ),
          mHoldDeleteSteps( -1 ), mFirstDeleteRepeatDone( false ),
          mLabelOnRight( false ),
          mLabelOnTop( false ),
          mSelectionStart( -1 ),
          mSelectionEnd( -1 ),
          mShiftPlusArrowsCanSelect( sPasteShortcutForNewFields ),
          mCursorFlashSteps( 0 ),
          mUsePasteShortcut( sPasteShortcutForNewFields ),
          mDragSelecting( false ),
          mDragSelectAnchor( 0 ),
          mDrawLabelWithShadow( inDrawLabelWithShadow ) {
    
    if( inLabelText != NULL ) {
        mLabelText = stringDuplicate( inLabelText );
        }
    
    if( inAllowedChars != NULL ) {
        mAllowedChars = stringDuplicate( inAllowedChars );
        }
    if( inForbiddenChars != NULL ) {
        mForbiddenChars = stringDuplicate( inForbiddenChars );
        }
    
    clearArrowRepeat();
        

    mCharWidth = mFont->getFontHeight();

    mBorderWide = mCharWidth * 0.25;

    mHigh = mFont->getFontHeight() + 2 * mBorderWide;

    char *fullString = new char[ mCharsWide + 1 ];

    unsigned char widestChar = 0;
    double width = 0;

    for( int c=32; c<128; c++ ) {
        unsigned char pc = processCharacter( c );

        if( pc != 0 ) {
            char s[2];
            s[0] = pc;
            s[1] = '\0';

            double thisWidth = mFont->measureString( s );
            
            if( thisWidth > width ) {
                width = thisWidth;
                widestChar = pc;    
                }
            }
        }
    
    


    for( int i=0; i<mCharsWide; i++ ) {
        fullString[i] = widestChar;
        }
    fullString[ mCharsWide ] = '\0';
    
    double fullStringWidth = mFont->measureString( fullString );

    delete [] fullString;

    mWide = fullStringWidth + 2 * mBorderWide;
    
    mDrawnTextX = - ( mWide / 2 - mBorderWide );

    mText[0] = '\0';
    }



TextField::~TextField() {
    if( this == sFocusedTextField ) {
        // we're focused, now nothing is focused
        sFocusedTextField = NULL;
        }

    delete [] mText;

    if( mLabelText != NULL ) {
        delete [] mLabelText;
        }

    if( mAllowedChars != NULL ) {
        delete [] mAllowedChars;
        }
    if( mForbiddenChars != NULL ) {
        delete [] mForbiddenChars;
        }

    if( mDrawnText != NULL ) {
        delete [] mDrawnText;
        }

    if( mHiddenSprite != NULL ) {
        freeSprite( mHiddenSprite );
        }
    }
    
    
void TextField::setLabelText( const char *inLabelText ) {
    if( mLabelText != NULL ) {
        delete [] mLabelText;
        }
    
    mLabelText = stringDuplicate( inLabelText );
    }



void TextField::setContentsHidden( char inHidden ) {
    mContentsHidden = inHidden;
    }




void TextField::setText( const char *inText ) {
    delete [] mText;
    mText = NULL;

    mSelectionStart = -1;
    mSelectionEnd = -1;
    mCursorPosition = 0;
    mTextLen = 0;
    if (inText == NULL || inText[0] == 0) {
        mText = new char[1];
        mText[0] = 0;
    } else {
        insertString(inText);
    }
    // insertString 可能因 maxLength 限制或非法 UTF-8 序列提前 break，
    // 此时 mText 仍为 NULL，下方 strlen(NULL) 会崩溃。补 NULL 守卫。
    if( mText == NULL ) {
        mText = new char[1];
        mText[0] = 0;
    }
    mTextLen = strlen( mText );

    mCursorPosition = mTextLen;
    // hold-downs broken
    mHoldDeleteSteps = -1;
    mFirstDeleteRepeatDone = false;

    clearArrowRepeat();
}



char *TextField::getText() {
    return stringDuplicate( mText );
    }



void TextField::setMaxLength( int inLimit ) {
    mMaxLength = inLimit;
    }



int TextField::getMaxLength() {
    return mMaxLength;
    }



char TextField::isAtLimit() {
    if( mMaxLength == -1 ) {
        return false;
        }
    else {
        return ( mTextLen == mMaxLength );
        }
    }
    



void TextField::setActive( char inActive ) {
    mActive = inActive;
    }



char TextField::isActive() {
    return mActive;
    }
        


void TextField::step() {

    mCursorFlashSteps ++;

    if( mHoldDeleteSteps > -1 ) {
        mHoldDeleteSteps ++;

        int stepsBetween = sDeleteFirstDelaySteps;
        
        if( mFirstDeleteRepeatDone ) {
            stepsBetween = sDeleteNextDelaySteps;
            }
        
        if( mHoldDeleteSteps > stepsBetween ) {
            // delete repeat
            mHoldDeleteSteps = 0;
            mFirstDeleteRepeatDone = true;
            
            deleteHit();
            }
        }


    for( int i=0; i<2; i++ ) {
        
        if( mHoldArrowSteps[i] > -1 ) {
            mHoldArrowSteps[i] ++;

            int stepsBetween = sDeleteFirstDelaySteps;
        
            if( mFirstArrowRepeatDone[i] ) {
                stepsBetween = sDeleteNextDelaySteps;
                }
        
            if( mHoldArrowSteps[i] > stepsBetween ) {
                // arrow repeat
                mHoldArrowSteps[i] = 0;
                mFirstArrowRepeatDone[i] = true;
            
                switch( i ) {
                    case 0:
                        leftHit();
                        break;
                    case 1:
                        rightHit();
                        break;
                    }
                }
            }
        }


    }

        
        
void TextField::draw() {
    
    if( mFocused ) {    
        setDrawColor( 1, 1, 1, 1 );
        }
    else {
        setDrawColor( 0.5, 0.5, 0.5, 1 );
        }
    

    drawRect( - mWide / 2, - mHigh / 2, 
              mWide / 2, mHigh / 2 );
    
    setDrawColor( 0.25, 0.25, 0.25, 1 );
    double pixWidth = mCharWidth / 8;


    double rectStartX = - mWide / 2 + pixWidth;
    double rectStartY = - mHigh / 2 + pixWidth;

    double rectEndX = mWide / 2 - pixWidth;
    double rectEndY = mHigh / 2 - pixWidth;

    double middleWidth = mWide - 2 * pixWidth;
    
    drawRect( rectStartX, rectStartY,
              rectEndX, rectEndY );
    
    setDrawColor( 1, 1, 1, 1 );

    if( mContentsHidden && mHiddenSprite != NULL ) {
        startAddingToStencil( false, true );

        drawRect( rectStartX, rectStartY,
                  rectEndX, rectEndY );
        startDrawingThroughStencil();
        
        doublePair pos = { 0, 0 };
        
        drawSprite( mHiddenSprite, pos );
        
        stopStencil();
        }
    


    
    if( mLabelText != NULL ) {
        TextAlignment a = alignRight;
        double xPos = -mWide/2 - mBorderWide;
        
        double yPos = 0;
        
        if( mLabelOnTop ) {
            xPos += mBorderWide + pixWidth;
            yPos = mHigh / 2 + 2 * mBorderWide;
            }

        if( mLabelOnRight ) {
            a = alignLeft;
            xPos = -xPos;
            }
        
        if( mLabelOnTop ) {
            // reverse align if on top
            if( a == alignLeft ) {
                a = alignRight;
                }
            else {
                a = alignLeft;
                }
            }
        
        doublePair labelPos = { xPos, yPos };

        if( mDrawLabelWithShadow ) {
            setDrawColor( 0, 0, 0, 1 );
            doublePair shadowOffset = {-2, 2};
            mFont->drawString( mLabelText, add(labelPos, shadowOffset), a );
            setDrawColor( 1, 1, 1, 1 );
            }
        
        mFont->drawString( mLabelText, labelPos, a );
        }
    
    
    if( mContentsHidden ) {
        return;
        }


    doublePair textPos = { - mWide/2 + mBorderWide, 0 };


    char tooLongFront = false;
    char tooLongBack = false;
    
    mCursorDrawPosition = mCursorPosition;


    char *textBeforeCursorBase = stringDuplicate( mText );
    char *textAfterCursorBase = stringDuplicate( mText );
    
    char *textBeforeCursor = textBeforeCursorBase;
    char *textAfterCursor = textAfterCursorBase;

    textBeforeCursor[ mCursorPosition ] = '\0';
    
    textAfterCursor = &( textAfterCursor[ mCursorPosition ] );
    /*
    if( mFont->measureString( mText ) > mWide - 2 * mBorderWide ) {
        
        if( mFont->measureString( textBeforeCursor ) > 
            mWide / 2 - mBorderWide
            &&
            mFont->measureString( textAfterCursor ) > 
            mWide / 2 - mBorderWide ) {

            // trim both ends

            while( mFont->measureString( textBeforeCursor ) > 
                   mWide / 2 - mBorderWide ) {
                
                tooLongFront = true;
                
                textBeforeCursor = &( textBeforeCursor[1] );
                
                mCursorDrawPosition --;
            }
        
            while( mFont->measureString( textAfterCursor ) > 
                   mWide / 2 - mBorderWide ) {
                
                tooLongBack = true;
                
                textAfterCursor[ strlen( textAfterCursor ) - 1 ] = '\0';
            }
        }
        else if( mFont->measureString( textBeforeCursor ) > 
                 mWide / 2 - mBorderWide ) {

            // just trim front
            char *sumText = concatonate( textBeforeCursor, textAfterCursor );
            
            while( mFont->measureString( sumText ) > 
                   mWide - 2 * mBorderWide ) {
                
                tooLongFront = true;
                
                textBeforeCursor = &( textBeforeCursor[1] );
                
                mCursorDrawPosition --;
                
                delete [] sumText;
                sumText = concatonate( textBeforeCursor, textAfterCursor );
            }
            delete [] sumText;
        }    
        else if( mFont->measureString( textAfterCursor ) > 
                 mWide / 2 - mBorderWide ) {
            
            // just trim back
            char *sumText = concatonate( textBeforeCursor, textAfterCursor );

            while( mFont->measureString( sumText ) > 
                   mWide - 2 * mBorderWide ) {
                
                tooLongBack = true;
                
                textAfterCursor[ strlen( textAfterCursor ) - 1 ] = '\0';
                delete [] sumText;
                sumText = concatonate( textBeforeCursor, textAfterCursor );
            }
            delete [] sumText;
        }
    }
    */
    
    if( mDrawnText != NULL ) {
        delete [] mDrawnText;
    }
    
    mDrawnText = concatonate( textBeforeCursor, textAfterCursor );

    char leftAlign = true;
    char cursorCentered = false;
    doublePair centerPos = { 0, 0 };
    
    if( ! tooLongFront ) {
        mFont->drawString( mDrawnText, textPos, alignLeft );
        mDrawnTextX = textPos.x;
    }
    else if( tooLongFront && ! tooLongBack ) {
        
        leftAlign = false;

        doublePair textPos2 = { mWide/2 - mBorderWide, 0 };

        mFont->drawString( mDrawnText, textPos2, alignRight );
        mDrawnTextX = textPos2.x - mFont->measureString( mDrawnText );
    }
    else {
        // text around perfectly centered cursor
        cursorCentered = true;
        
        double beforeLength = mFont->measureString( textBeforeCursor );
        
        double xDiff = centerPos.x - ( textPos.x + beforeLength );
        
        doublePair textPos2 = textPos;
        textPos2.x += xDiff;

        mFont->drawString( mDrawnText, textPos2, alignLeft );
        mDrawnTextX = textPos2.x;
    }


    if( isAnythingSelected() ) {
        fixSelectionStartEnd();

        char *beforeSelection = stringDuplicate( mText );
        beforeSelection[ mSelectionStart ] = '\0';

        char *selectionText = stringDuplicate( mText );
        selectionText[ mSelectionEnd ] = '\0';

        double selectionStartX =
            mDrawnTextX + mFont->measureString( beforeSelection );
        double selectionEndX =
            selectionStartX +
            mFont->measureString( &( selectionText[ mSelectionStart ] ) );

        delete [] beforeSelection;
        delete [] selectionText;

        // highlight bar behind the selected part of the text
        setDrawColor( 0.30, 0.55, 0.85, 1 );
        drawRect( selectionStartX, rectStartY,
                  selectionEndX, rectEndY );

        // redraw the text on top of the highlight
        setDrawColor( 1, 1, 1, 1 );
        mFont->drawString( mDrawnText, textPos, alignLeft );
        }


    double shadeWidth = 4 * mCharWidth;
    
    if( shadeWidth > middleWidth / 2 ) {
        shadeWidth = middleWidth / 2;
    }

    if( tooLongFront ) {
        // draw shaded overlay over left of string
        
        double verts[] = { rectStartX, rectStartY,
                           rectStartX, rectEndY,
                           rectStartX + shadeWidth, rectEndY,
                           rectStartX + shadeWidth, rectStartY };
        float vertColors[] = { 0.25, 0.25, 0.25, 1,
                               0.25, 0.25, 0.25, 1,
                               0.25, 0.25, 0.25, 0,
                               0.25, 0.25, 0.25, 0 };

        drawQuads( 1, verts , vertColors );
    }
    if( tooLongBack ) {
        // draw shaded overlay over right of string
        
        double verts[] = { rectEndX - shadeWidth, rectStartY,
                           rectEndX - shadeWidth, rectEndY,
                           rectEndX, rectEndY,
                           rectEndX, rectStartY };
        float vertColors[] = { 0.25, 0.25, 0.25, 0,
                               0.25, 0.25, 0.25, 0,
                               0.25, 0.25, 0.25, 1,
                               0.25, 0.25, 0.25, 1 };

        drawQuads( 1, verts , vertColors );
    }
    
    if( mFocused && mCursorDrawPosition > -1 ) {            
        // make measurement to draw cursor

        char *beforeCursorText = stringDuplicate( mDrawnText );
        
        beforeCursorText[ mCursorDrawPosition ] = '\0';
        
        
        double cursorXOffset;

        if( cursorCentered ) {
            cursorXOffset = mWide / 2 - mBorderWide;
            }
        else if( leftAlign ) {
            cursorXOffset = mFont->measureString( textBeforeCursor );
            if( cursorXOffset == 0 ) {
                cursorXOffset -= pixWidth;
                }
            }
        else {
            double afterLength = mFont->measureString( textAfterCursor );
            cursorXOffset = ( mWide - 2 * mBorderWide ) - afterLength;

            if( afterLength > 0 ) {
                cursorXOffset -= pixWidth;
                }
            }
        

        
        delete [] beforeCursorText;
        
        setDrawColor( 0, 0, 0, 0.5 );
        
        drawRect( textPos.x + cursorXOffset, 
                  rectStartY - pixWidth,
                  textPos.x + cursorXOffset + pixWidth, 
                  rectEndY + pixWidth );
        }
    
    
    if( ! mActive ) {
        setDrawColor( 0, 0, 0, 0.5 );
        // dark overlay
        drawRect( - mWide / 2, - mHigh / 2, 
                  mWide / 2, mHigh / 2 );
        }
        

    delete [] textBeforeCursorBase;
    delete [] textAfterCursorBase;

}


char TextField::isInside( float inX, float inY ) {
    return fabs( inX ) < mWide / 2 &&
        fabs( inY ) < mHigh / 2;
    }


void TextField::pointerMove( float inX, float inY ) {
    mHover = isInside( inX, inY );
    }


void TextField::placeCursorAtX( float inX ) {
    if( mDrawnText == NULL ) {
        return;
        }

    int bestCursorDrawPosition = mCursorDrawPosition;
    double bestDistance = mWide * 2;

    int drawnTextLength = strlen( mDrawnText );

    // find gap between drawn letters that is closest to clicked x

    for( int i=0; i<=drawnTextLength; i++ ) {

        char *textCopy = stringDuplicate( mDrawnText );

        textCopy[i] = '\0';

        double thisGapX =
            mDrawnTextX +
            mFont->measureString( textCopy ) +
            mFont->getCharSpacing() / 2;

        delete [] textCopy;

        double thisDistance = fabs( thisGapX - inX );

        if( thisDistance < bestDistance ) {
            bestCursorDrawPosition = i;
            bestDistance = thisDistance;
            }
        }

    // snap to a UTF-8 character boundary
    // (don't stop in the middle of a multi-byte character)
    while( bestCursorDrawPosition > 0 &&
           bestCursorDrawPosition < drawnTextLength &&
           ( (unsigned char)mDrawnText[ bestCursorDrawPosition ] & 0xC0 ) == 0x80 ) {
        bestCursorDrawPosition --;
        }

    int cursorDelta = bestCursorDrawPosition - mCursorDrawPosition;

    mCursorPosition += cursorDelta;
    }


void TextField::pointerDown( float inX, float inY ) {
    if( mIgnoreMouse || mIgnoreEvents ) {
        return;
        }

    if( inX > - mWide / 2 &&
        inX < + mWide / 2 &&
        inY > - mHigh / 2 &&
        inY < + mHigh / 2 ) {

        char wasHidden = mContentsHidden;

        focus();

        if( wasHidden ) {
            // don't adjust cursor from where it was
            }
        else {
            // a click clears the selection and starts a new one
            // that mouse dragging can extend
            mSelectionStart = -1;
            mSelectionEnd = -1;

            placeCursorAtX( inX );

            mDragSelectAnchor = mCursorPosition;
            mDragSelecting = true;
            }
        }
    }


void TextField::pointerDrag( float inX, float inY ) {
    if( !mDragSelecting ) {
        return;
        }

    placeCursorAtX( inX );

    if( mCursorPosition != mDragSelectAnchor ) {
        mSelectionStart = mDragSelectAnchor;
        mSelectionEnd = mCursorPosition;
        fixSelectionStartEnd();
        }
    else {
        mSelectionStart = -1;
        mSelectionEnd = -1;
        }
    }


void TextField::pointerUp( float inX, float inY ) {
    if( mIgnoreMouse || mIgnoreEvents ) {
        return;
        }

    int mouseButton = getLastMouseButton();
    if ( mouseButton == MouseButton::WHEELUP || mouseButton == MouseButton::WHEELDOWN ) { return; }

    if( mDragSelecting ) {
        // finish a drag selection
        mDragSelecting = false;

        if( ! isAnythingSelected() ) {
            mSelectionStart = -1;
            mSelectionEnd = -1;
            }
        return;
        }

    if( inX > - mWide / 2 &&
        inX < + mWide / 2 &&
        inY > - mHigh / 2 &&
        inY < + mHigh / 2 ) {

        char wasHidden = mContentsHidden;

        focus();

        if( wasHidden ) {
            // don't adjust cursor from where it was
            }
        else {
            
            int bestCursorDrawPosition = mCursorDrawPosition;
            double bestDistance = mWide * 2;
            
            int drawnTextLength = strlen( mDrawnText );
            
            // find gap between drawn letters that is closest to clicked x
            
            for( int i=0; i<=drawnTextLength; i++ ) {
                
                char *textCopy = stringDuplicate( mDrawnText );
                
                textCopy[i] = '\0';
                
                double thisGapX = 
                    mDrawnTextX + 
                    mFont->measureString( textCopy ) +
                    mFont->getCharSpacing() / 2;
                
                delete [] textCopy;
                
                double thisDistance = fabs( thisGapX - inX );
                
                if( thisDistance < bestDistance ) {
                    bestCursorDrawPosition = i;
                    bestDistance = thisDistance;
                    }
                }

            // snap to a UTF-8 character boundary
            // (don't stop in the middle of a multi-byte character)
            while( bestCursorDrawPosition > 0 &&
                   bestCursorDrawPosition < drawnTextLength &&
                   ( (unsigned char)mDrawnText[ bestCursorDrawPosition ] & 0xC0 ) == 0x80 ) {
                bestCursorDrawPosition --;
                }

            int cursorDelta = bestCursorDrawPosition - mCursorDrawPosition;

            mCursorPosition += cursorDelta;
            }
        }
    }


// unicode TextField::processCharacter(unicode ch) {
//     unicode pch = ch;
//     if (ch < 128) {
//         pch = processCharacter(ch);
//     }
//     return pch;
// }

unsigned char TextField::processCharacter( unsigned char inASCII ) {
    if( mForbiddenChars != NULL ) {
        int num = strlen( mForbiddenChars );
            
        for( int i=0; i<num; i++ ) {
            if( mForbiddenChars[i] == inASCII ) {
                return 0;
                }
            }
        }

    unsigned char processedChar = inASCII;
        
    if( mForceCaps ) {
        processedChar = toupper( inASCII );
    }
        

    if( mAllowedChars != NULL ) {
        int num = strlen( mAllowedChars );
            
        char allowed = false;
            
        for( int i=0; i<num; i++ ) {
            if( mAllowedChars[i] == processedChar ) {
                allowed = true;
                break;
            }
        }

        if( !allowed ) {
            return 0;
            }
    }
    else {
        // no allowed list specified 
        
        if( processedChar == '\r' || processedChar == '\n' ) {
            // \r and \n only permitted if it is listed explicitly
            return 0;
            }
        }
        

    return processedChar;
}



void TextField::insertCharacter( unsigned char inASCII ) {
    
    if( isAnythingSelected() ) {
        // delete selected first
        deleteHit();
    }
    if (mText == NULL) {
        mText = autoSprintf( "%c", inASCII);
        mTextLen = 1;
        mCursorPosition++;
        return;
    }
    // add to it
    char *oldText = mText;
    
    if( mMaxLength != -1 &&
        strlen( oldText ) >= (unsigned int) mMaxLength ) {
        // max length hit, don't add it
        return;
    }
    
    char *preCursor = stringDuplicate( mText );
    preCursor[ mCursorPosition ] = '\0';
    char *postCursor = &( mText[ mCursorPosition ] );
    
    mText = autoSprintf( "%s%c%s", 
                         preCursor, inASCII, postCursor );
    mTextLen = strlen( mText );

    delete [] preCursor;
    
    delete [] oldText;
    
    mCursorPosition++;
}



void TextField::insertString(const char *inString ) {
    if( isAnythingSelected() ) {
        // delete selected first
        deleteHit();
    }
    unsigned char *p = (unsigned char*)inString;
    int charWidth = -1;
    while (*p != 0) {
        if ((*p & 0x80) == 0) {  // 1 byte
            charWidth = 1;
        } else if ((*p & 0xE0) == 0xC0) { // 2 bytes
            charWidth = 2;
        } else if ((*p & 0xF0) == 0xE0) { // 3 bytes
            charWidth = 3;
        } else if ((*p & 0xF8) == 0xF0) { // 3 bytes
            charWidth = 4;
        } else {
            charWidth = -1;
        }
        if (charWidth < 0 || (mMaxLength > 0 && (mTextLen + charWidth > mMaxLength)))
            break;
        // refuse invalid UTF-8 sequences (e.g. ANSI clipboard text)
        // instead of inserting them as garbage
        bool sequenceValid = true;
        for (int i = 1; i < charWidth; i++) {
            if ((p[i] & 0xC0) != 0x80) {
                sequenceValid = false;
                break;
            }
        }
        if (!sequenceValid) {
            // skip just this byte and keep scanning
            p++;
            continue;
        }
        bool insertSuccess = true;
        for (int i=0; i<charWidth; i++){
            unsigned char processedChar = processCharacter( *(p+i));
            if( processedChar != 0 ) {
                insertCharacter( processedChar );
            } else {
                if (i>0) {
                    mSelectionStart = mCursorPosition - i;
                    mSelectionEnd = mCursorPosition;
                    deleteHit(); // 删除已经插入的字符
                    mSelectionStart = -1;
                    mSelectionEnd = -1;
                }
                insertSuccess = false;
                break;
            }
        }
        p += charWidth;
    }
}



int TextField::getCursorPosition() {
    return mCursorPosition;
    }


void TextField::cursorReset() {
    mCursorPosition = 0;
    }



void TextField::setIgnoreArrowKeys( char inIgnore ) {
    mIgnoreArrowKeys = inIgnore;
    }



void TextField::setIgnoreMouse( char inIgnore ) {
    mIgnoreMouse = inIgnore;
    }



double TextField::getRightEdgeX() {
    
    return mX + mWide / 2;
    }



double TextField::getLeftEdgeX() {
    
    return mX - mWide / 2;
    }



double TextField::getWidth() {
    
    return mWide;
    }



void TextField::setWidth( double inWide ) {
    
    mWide = inWide;
    }
    
    
void TextField::setHigh( double inHigh ) {
    
    mHigh = inHigh;
    }



void TextField::setFireOnAnyTextChange( char inFireOnAny ) {
    mFireOnAnyChange = inFireOnAny;
    }


void TextField::setFireOnLoseFocus( char inFireOnLeave ) {
    mFireOnLeave = inFireOnLeave;
    }

void TextField::keyDown( unsigned char inASCII ) {
    if( !mFocused ) {
        return;
        }
    mCursorFlashSteps = 0;

    if( isCommandKeyDown() ) {
        // not a normal key stroke (command key)
        // ignore it as input

        if( mUsePasteShortcut && isClipboardSupported() ) {

            if( inASCII == 'v' || inASCII == 22 ) {
                // ctrl-v is SYN on some platforms

                // paste!
                char *clipboardText = getClipboardText();
                insertString(clipboardText);
                delete [] clipboardText;

                mHoldDeleteSteps = -1;
                mFirstDeleteRepeatDone = false;

                clearArrowRepeat();

                if( mFireOnAnyChange ) {
                    fireActionPerformed( this );
                }
                return;
                }
            else if( inASCII == 'c' || inASCII == 3 ) {
                // ctrl-c is ETX on some platforms

                // copy!
                // selected text, or the whole field if nothing is selected
                fixSelectionStartEnd();
                char *selectedText = getSelectedText();
                if( selectedText != NULL ) {
                    setClipboardText( selectedText );
                    delete [] selectedText;
                    }
                else {
                    setClipboardText( mText );
                    }
                return;
                }
            else if( inASCII == 'x' || inASCII == 24 ) {
                // ctrl-x is CAN on some platforms

                // cut!
                // only when something is selected
                if( isAnythingSelected() ) {
                    fixSelectionStartEnd();
                    char *selectedText = getSelectedText();
                    setClipboardText( selectedText );
                    delete [] selectedText;

                    deleteHit();

                    mHoldDeleteSteps = -1;
                    mFirstDeleteRepeatDone = false;
                    }
                return;
                }
            else if( inASCII == 'a' || inASCII == 1 ) {
                // ctrl-a is SOH on some platforms

                // select all!
                mSelectionStart = 0;
                mSelectionEnd = mTextLen;
                mSelectionAdjusting = &mSelectionEnd;
                mCursorPosition = mTextLen;
                return;
                }
            }

        // but ONLY if it's an alphabetical key (A-Z,a-z)
        // Some international keyboards use ALT to type certain symbols

        if( ( inASCII >= 'A' && inASCII <= 'Z' )
            ||
            ( inASCII >= 'a' && inASCII <= 'z' ) ) {

            return;
            }

        }

    if( inASCII == 127 || inASCII == 8 ) {
        // delete
        // (deleteHit removes a whole UTF-8 character at a time)
        deleteHit();

        mHoldDeleteSteps = 0;

        clearArrowRepeat();
        }
    else if( inASCII == 13 ) {
        // enter hit in field
        unsigned char processedChar = processCharacter( inASCII );    

        if( processedChar != 0 ) {
            // newline is allowed
            insertCharacter( processedChar );
            mHoldDeleteSteps = -1;
            mFirstDeleteRepeatDone = false;
            
            clearArrowRepeat();
            
            if( mFireOnAnyChange ) {
                fireActionPerformed( this );
                }
            }
        else {
            // newline not allowed in this field
            fireActionPerformed( this );
            }
        }
    else if( inASCII >= 32 ) {

        unsigned char processedChar = processCharacter( inASCII );    

        if( processedChar != 0 ) {

            insertCharacter( processedChar );
            }
        
        mHoldDeleteSteps = -1;
        mFirstDeleteRepeatDone = false;

        clearArrowRepeat();

        if( mFireOnAnyChange ) {
            fireActionPerformed( this );
            }
        }    
    }



void TextField::keyUp( unsigned char inASCII ) {
    if( inASCII == 127 || inASCII == 8 ) {
        // end delete hold down
        mHoldDeleteSteps = -1;
        mFirstDeleteRepeatDone = false;
        }
    }



void TextField::deleteHit() {
    if( mCursorPosition > 0 || isAnythingSelected() ) {
        mCursorFlashSteps = 0;
    
        int newCursorPos = mCursorPosition - 1;


        if( isAnythingSelected() ) {
            // selection delete
            
            mCursorPosition = mSelectionEnd;
            
            newCursorPos = mSelectionStart;

            mSelectionStart = -1;
            mSelectionEnd = -1;
            }
        else if( isCommandKeyDown() ) {
            // word delete 

            newCursorPos = mCursorPosition;

            // skip non-space, non-newline characters
            while( newCursorPos > 0 &&
                   mText[ newCursorPos - 1 ] != ' ' &&
                   mText[ newCursorPos - 1 ] != '\r' ) {
                newCursorPos --;
                }
        
            // skip space and newline characters
            while( newCursorPos > 0 &&
                   ( mText[ newCursorPos - 1 ] == ' ' ||
                     mText[ newCursorPos - 1 ] == '\r' ) ) {
                newCursorPos --;
                }
            }
        else {
            // plain backspace:
            // expand to remove the whole UTF-8 character
            // containing the byte before the cursor
            newCursorPos = utf8CharStart( mText, newCursorPos );
            }
        
        // section cleared no matter what when delete is hit
        mSelectionStart = -1;
        mSelectionEnd = -1;


        char *oldText = mText;
        
        char *preCursor = stringDuplicate( mText );
        preCursor[ newCursorPos ] = '\0';
        char *postCursor = &( mText[ mCursorPosition ] );

        mText = autoSprintf( "%s%s", preCursor, postCursor );
        mTextLen = strlen( mText );
        
        delete [] preCursor;

        delete [] oldText;

        mCursorPosition = newCursorPos;

        if( mFireOnAnyChange ) {
            fireActionPerformed( this );
            }
        }
    }



void TextField::clearArrowRepeat() {
    for( int i=0; i<2; i++ ) {
        mHoldArrowSteps[i] = -1;
        mFirstArrowRepeatDone[i] = false;
        }
    }



void TextField::leftHit() {
    mCursorFlashSteps = 0;
    
    if( isShiftKeyDown() && mShiftPlusArrowsCanSelect ) {
        if( !isAnythingSelected() ) {
            mSelectionStart = mCursorPosition;
            mSelectionEnd = mCursorPosition;
            mSelectionAdjusting = &mSelectionStart;
            }
        else {
            mCursorPosition = *mSelectionAdjusting;
            }
        }

    if( ! isShiftKeyDown() ) {
        if( isAnythingSelected() ) {
            mCursorPosition = mSelectionStart + 1;
        }

        mSelectionStart = -1;
        mSelectionEnd = -1;
    }

    if( isCommandKeyDown() ) {
        // word jump 

        // skip non-space, non-newline characters
        while( mCursorPosition > 0 &&
               mText[ mCursorPosition - 1 ] != ' ' &&
               mText[ mCursorPosition - 1 ] != '\r' ) {
            mCursorPosition --;
            }
        
        // skip space and newline characters
        while( mCursorPosition > 0 &&
               ( mText[ mCursorPosition - 1 ] == ' ' ||
                 mText[ mCursorPosition - 1 ] == '\r' ) ) {
            mCursorPosition --;
            }
        
        }
    else {
        mCursorPosition --;
        // snap to the start of the UTF-8 character we stepped into
        mCursorPosition = utf8CharStart( mText, mCursorPosition );
        if( mCursorPosition < 0 ) {
            mCursorPosition = 0;
            }
        }

    if( isShiftKeyDown() && mShiftPlusArrowsCanSelect ) {
        *mSelectionAdjusting = mCursorPosition;
        fixSelectionStartEnd();
        }

    }



void TextField::rightHit() {
    mCursorFlashSteps = 0;
    
    if( isShiftKeyDown() && mShiftPlusArrowsCanSelect ) {
        if( !isAnythingSelected() ) {
            mSelectionStart = mCursorPosition;
            mSelectionEnd = mCursorPosition;
            mSelectionAdjusting = &mSelectionEnd;
            }
        else {
            mCursorPosition = *mSelectionAdjusting;
            }
        }
    
    if( ! isShiftKeyDown() ) {
        if( isAnythingSelected() ) {
            mCursorPosition = mSelectionEnd - 1;
            }
            
        mSelectionStart = -1;
        mSelectionEnd = -1;
        }

    if( isCommandKeyDown() ) {
        // word jump 
        int textLen = strlen( mText );
        
        // skip space and newline characters
        while( mCursorPosition < textLen &&
               ( mText[ mCursorPosition ] == ' ' ||
                 mText[ mCursorPosition ] == '\r'  ) ) {
            mCursorPosition ++;
            }

        // skip non-space and non-newline characters
        while( mCursorPosition < textLen &&
               mText[ mCursorPosition ] != ' ' &&
               mText[ mCursorPosition ] != '\r' ) {
            mCursorPosition ++;
            }
        
        
        }
    else {
        mCursorPosition ++;
        // skip UTF-8 continuation bytes (10xxxxxx)
        // to land on the next character boundary
        while( mCursorPosition < (int)strlen( mText ) &&
               ( (unsigned char)mText[ mCursorPosition ] & 0xC0 ) == 0x80 ) {
            mCursorPosition ++;
            }
        if( mCursorPosition > (int)strlen( mText ) ) {
            mCursorPosition = strlen( mText );
            }
        }

    if( isShiftKeyDown() && mShiftPlusArrowsCanSelect ) {
        *mSelectionAdjusting = mCursorPosition;
        fixSelectionStartEnd();
        }
    
    }




void TextField::specialKeyDown( int inKeyCode ) {
    if( !mFocused ) {
        return;
        }
    
    mCursorFlashSteps = 0;
    
    switch( inKeyCode ) {
        case MG_KEY_LEFT:
            if( ! mIgnoreArrowKeys ) {    
                leftHit();
                clearArrowRepeat();
                mHoldArrowSteps[0] = 0;
                }
            break;
        case MG_KEY_RIGHT:
            if( ! mIgnoreArrowKeys ) {
                rightHit(); 
                clearArrowRepeat();
                mHoldArrowSteps[1] = 0;
                }
            break;
        default:
            break;
        }
    
    }



void TextField::specialKeyUp( int inKeyCode ) {
    if( inKeyCode == MG_KEY_LEFT ) {
        mHoldArrowSteps[0] = -1;
        mFirstArrowRepeatDone[0] = false;
        }
    else if( inKeyCode == MG_KEY_RIGHT ) {
        mHoldArrowSteps[1] = -1;
        mFirstArrowRepeatDone[1] = false;
        }
    }



void TextField::setIgnoredKey( unsigned char inASCII ) {
    
    std::string newChars( mForbiddenChars );
    newChars.push_back( inASCII );
    if( mForbiddenChars != NULL ) {
        delete [] mForbiddenChars;
        mForbiddenChars = NULL;
        }
    mForbiddenChars = stringDuplicate( newChars.c_str() );
    
    }



void TextField::focus() {
    
    if( sFocusedTextField != NULL && sFocusedTextField != this ) {
        // unfocus last focused
        sFocusedTextField->unfocus();
        }
        
    DropdownList::unfocusAll();

    mFocused = true;
    sFocusedTextField = this;

    mContentsHidden = false;
    }



void TextField::unfocus() {
    mFocused = false;
 
    // hold-down broken if not focused
    mHoldDeleteSteps = -1;
    mFirstDeleteRepeatDone = false;

    clearArrowRepeat();

    if( sFocusedTextField == this ) {
        sFocusedTextField = NULL;
        if( mFireOnLeave ) {
            fireActionPerformed( this );
            }
        }    
    }



char TextField::isFocused() {
    return mFocused;
    }



void TextField::setDeleteRepeatDelays( int inFirstDelaySteps,
                                       int inNextDelaySteps ) {
    sDeleteFirstDelaySteps = inFirstDelaySteps;
    sDeleteNextDelaySteps = inNextDelaySteps;
    }



char TextField::isAnyFocused() {
    if( sFocusedTextField != NULL ) {
        return true;
        }
    return false;
    }


        
void TextField::unfocusAll() {
    
    if( sFocusedTextField != NULL ) {
        // unfocus last focused
        sFocusedTextField->unfocus();
        }

    sFocusedTextField = NULL;
    }




int TextField::getInt() {
    char *text = getText();
    
    int i = 0;
    
    sscanf( text, "%d", &i );
    
    delete [] text;
            
    return i;
    }

        
        
float TextField::getFloat() {
    char *text = getText();
            
    float f = 0;
    
    sscanf( text, "%f", &f );
    
    delete [] text;
    
    return f;
    }



void TextField::setInt( int inI ) {
    char *text = autoSprintf( "%d", inI );
    
    setText( text );
    delete [] text;
    }

        

void TextField::setFloat( float inF, int inDigitsAfterDecimal, 
                          char inTrimZeros ) {

    char *formatString;
    
    if( inDigitsAfterDecimal == -1 ) {
        formatString = stringDuplicate( "%f" );
        }
    else {
        formatString = autoSprintf( "%%.%df", inDigitsAfterDecimal );
        }

    char *text = autoSprintf( formatString, inF );
    
    if( inTrimZeros && strstr( text, "." ) != NULL ) {
        int index = strlen( text ) - 1;
        
        while( index > 1 && text[index] == '0' ) {
            if( text[index-1] == '.' ) {
                // leave one zero after .
                break;
                }
            text[index] = '\0';
            index --;
            }
        }

    delete [] formatString;

    setText( text );
    delete [] text;
    }




void TextField::setLabelSide( char inLabelOnRight ) {
    mLabelOnRight = inLabelOnRight;
    }



void TextField::setLabelTop( char inLabelOnTop ) {
    mLabelOnTop = inLabelOnTop;
    }


        
char TextField::isAnythingSelected() {
    return 
        ( mSelectionStart != -1 && 
          mSelectionEnd != -1 &&
          mSelectionStart != mSelectionEnd );
}



char *TextField::getSelectedText() {

    if( ! isAnythingSelected() ) {
        return NULL;
        }
    
    char *textCopy = stringDuplicate( mText );

    textCopy[ mSelectionEnd ] = '\0';
    
    char *startPointer = &( textCopy[ mSelectionStart ] );
    
    char *returnVal = stringDuplicate( startPointer );
    
    delete [] textCopy;
    
    return returnVal;
    }



void TextField::fixSelectionStartEnd() {
    if( mSelectionEnd < mSelectionStart ) {
        int temp = mSelectionEnd;
        mSelectionEnd = mSelectionStart;
        mSelectionStart = temp;

        if( mSelectionAdjusting == &mSelectionStart ) {
            mSelectionAdjusting = &mSelectionEnd;
            }
        else if( mSelectionAdjusting == &mSelectionEnd ) {
            mSelectionAdjusting = &mSelectionStart;
            }
        }
    else if( mSelectionEnd == mSelectionStart ) {
        mSelectionAdjusting = &mSelectionEnd;
        }
    
    }



void TextField::setShiftArrowsCanSelect( char inCanSelect ) {
    mShiftPlusArrowsCanSelect = inCanSelect;
    }



void TextField::usePasteShortcut( char inShortcutOn ) {
    mUsePasteShortcut = inShortcutOn;
    }


void TextField::setPasteShortcutForNewFields( char inOn ) {
    sPasteShortcutForNewFields = inOn;
    }


char TextField::isMouseOver() {
    return mHover;
    }
