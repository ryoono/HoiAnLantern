/*
 * ============================================================
 *  Hoi An Lantern
 *  PIC16F18313 LED Flicker Program
 * ============================================================
 *
 * MCU      : PIC16F18313
 * Compiler : MPLAB XC8
 * Supply   : AA battery x 3 (nominal 4.5 V)
 * Clock    : Internal HFINTOSC 1 MHz
 *
 * ------------------------------------------------------------
 * Pin assignment
 * ------------------------------------------------------------
 *
 * PIC16F18313
 *
 * Pin 1 : VDD
 * Pin 2 : RA5 / PWM6 -> 100R -> 2SK4017 Gate (LED CH2)
 * Pin 3 : RA4 / PWM5 -> 100R -> 2SK4017 Gate (LED CH1)
 * Pin 4 : MCLR/VPP
 * Pin 5 : RA2         -> unused
 * Pin 6 : RA1/ICSPCLK
 * Pin 7 : RA0/ICSPDAT
 * Pin 8 : VSS
 *
 * 2SK4017 Gate-GND : 100kR pull-down
 *
 * LED:
 * +4.5V
 *   -> current limiting resistor (75ohm)
 *   -> LED (OSM2DK5111A-UV)
 *   -> 2SK4017 Drain
 *
 * 2SK4017 Source -> GND
 *
 * ------------------------------------------------------------
 * Flicker algorithm
 * ------------------------------------------------------------
 *
 * 2種類の間欠カオスを使用する。
 *
 * Chaos 1:
 *
 *   LED CH1 / CH2の逆相揺らぎ。
 *
 *   片方が明るくなると、
 *   もう片方が暗くなる。
 *
 *   炎の中で明るい位置が移動するような
 *   局所的な揺らぎを表現する。
 *
 *
 * Chaos 2:
 *
 *   2灯共通の全体輝度揺らぎ。
 *
 *   CH1 / CH2の逆相関係を維持したまま、
 *   ランタン全体を明るくしたり暗くしたりする。
 *
 *   炎そのものの強弱を表現する。
 *
 *
 * 最終出力:
 *
 *   gamma補正後の輝度にOUTPUT_GAINを掛け、
 *   現在の揺らぎの形を保ったまま最大輝度を引き上げる。
 *
 *   255を超えた場合は255へ飽和させるため、
 *   PWM Dutyは100%を超えない。
 *
 *
 * LEDの揺らぎ生成部分は以下の記事で紹介されている
 * 間欠カオスを用いた手法をベースにしている。
 *
 * https://www.creativity-ape.com/entry/2020/12/16/232928
 *
 * 元のArduino版から以下を変更している。
 *
 * ・float演算をQ15整数演算に変更
 * ・PIC16F18313のPWM5 / PWM6を使用
 * ・Timer2によるハードウェアPWM
 * ・delay()を使用しない
 * ・CPUは待機中Idleモードへ移行
 * ・不要な周辺モジュールをPMDで停止
 * ・逆相揺らぎとは独立した全体輝度用カオスを追加
 * ・最終出力ゲインを追加
 * ・出力ゲイン計算時はuint32_tを使用して
 *   16bitオーバーフローを防止
 * ・100%を超える場合は255へ飽和
 *
 * ============================================================
 */

#include <xc.h>
#include <stdint.h>


// ============================================================
// Configuration Bits
// ============================================================

// 外部発振器を使用しない
#pragma config FEXTOSC = OFF

// リセット後はHFINTOSC 1MHzで起動
#pragma config RSTOSC = HFINT1

// クロック出力を無効化
#pragma config CLKOUTEN = OFF

// ソフトウェアからのクロック切替を許可
#pragma config CSWEN = ON

// 外部クロックを使わないのでFail-Safe Clock Monitorは不要
#pragma config FCMEN = OFF

// MCLRを有効化
#pragma config MCLRE = ON

// 電源投入時にPower-up Timerを使用
#pragma config PWRTE = ON

// Watchdog Timerは使用しない
#pragma config WDTE = OFF

// Brown-out Resetを有効化
#pragma config BOREN = ON

// 低いBOR電圧を使用
#pragma config BORV = LOW

// PPSロック解除は1回のみ
#pragma config PPS1WAY = ON

// Stack Overflow / Underflow時にReset
#pragma config STVREN = ON

// デバッグ機能OFF
#pragma config DEBUG = OFF

// Low Voltage Programming有効
#pragma config LVP = ON

// Flash自己書込み保護なし
#pragma config WRT = OFF

// Program Memory保護なし
#pragma config CP = OFF

// EEPROM保護なし
#pragma config CPD = OFF


#define _XTAL_FREQ 1000000UL


// ============================================================
// User settings
// ============================================================

/*
 * ------------------------------------------------------------
 * LED1 / LED2間の逆相揺らぎの深さ
 * ------------------------------------------------------------
 *
 * FLICKER_DEPTH = 50 の場合、
 *
 * CH1:
 *   205 ～ 255
 *
 * CH2:
 *   255 ～ 205
 *
 * の範囲で逆方向に変化する。
 */
#define FLICKER_DEPTH              50u


/*
 * ------------------------------------------------------------
 * ランタン全体の揺らぎ範囲
 * ------------------------------------------------------------
 *
 * Q8形式:
 *
 * 256 = 100%
 * 230 = 約90%
 * 205 = 約80%
 * 180 = 約70%
 *
 * 初期設定:
 *
 *   約70% ～ 100%
 */
#define GLOBAL_BRIGHTNESS_MIN_Q8   180u
#define GLOBAL_BRIGHTNESS_MAX_Q8   256u


/*
 * ------------------------------------------------------------
 * 出力ゲイン
 * ------------------------------------------------------------
 *
 * gamma補正後の輝度を約1.098倍する。
 *
 * 281 / 256
 *
 *   ≒ 1.09765625
 *
 * 理論上255を超える場合があるため、
 * applyOutputGain()内で255へ飽和させる。
 *
 * 乗算にはuint32_tを使用する。
 *
 * 最大:
 *
 *   255 * 281
 *   = 71655
 *
 * これはuint16_tの最大65535を超えるため、
 * uint16_tで計算してはいけない。
 */
#define OUTPUT_GAIN_Q8             281u


/*
 * ------------------------------------------------------------
 * 全体の最大明るさ
 * ------------------------------------------------------------
 *
 * OUTPUT_GAIN適用後に使用する。
 *
 * 256 = 100%
 * 192 =  75%
 * 128 =  50%
 *  64 =  25%
 */
#define MASTER_BRIGHTNESS_Q8       256u


/*
 * ------------------------------------------------------------
 * 間欠カオスのthreshold
 * ------------------------------------------------------------
 *
 * 元Arduino版:
 *
 * threshold = 0.065
 *
 * Q15では
 *
 * 0.065 * 32768 ≒ 2130
 */
#define CHAOS_THRESHOLD            2130u

#define Q15_ONE                    32768UL
#define Q15_HALF                   16384u


/*
 * 擬似乱数初期値
 *
 * 2個のランタンに同じプログラムを書き込む場合は、
 * 2台目で変更すると揺らぎが一致しにくくなる。
 */
#define RNG_SEED                   0xA531u


// ============================================================
// Gamma correction table
// ============================================================

static const uint8_t gamma8[256] = {

    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,

    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 1, 1, 1, 1,

    1, 1, 1, 1, 1, 1, 1, 1,
    1, 2, 2, 2, 2, 2, 2, 2,

    2, 3, 3, 3, 3, 3, 3, 3,
    4, 4, 4, 4, 4, 5, 5, 5,

    5, 6, 6, 6, 6, 7, 7, 7,
    7, 8, 8, 8, 9, 9, 9, 10,

    10, 10, 11, 11, 11, 12, 12, 13,
    13, 13, 14, 14, 15, 15, 16, 16,

    17, 17, 18, 18, 19, 19, 20, 20,
    21, 21, 22, 22, 23, 24, 24, 25,

    25, 26, 27, 27, 28, 29, 29, 30,
    31, 32, 32, 33, 34, 35, 35, 36,

    37, 38, 39, 39, 40, 41, 42, 43,
    44, 45, 46, 47, 48, 49, 50, 50,

    51, 52, 54, 55, 56, 57, 58, 59,
    60, 61, 62, 63, 64, 66, 67, 68,

    69, 70, 72, 73, 74, 75, 77, 78,
    79, 81, 82, 83, 85, 86, 87, 89,

    90, 92, 93, 95, 96, 98, 99, 101,
    102, 104, 105, 107, 109, 110, 112, 114,

    115, 117, 119, 120, 122, 124, 126, 127,
    129, 131, 133, 135, 137, 138, 140, 142,

    144, 146, 148, 150, 152, 154, 156, 158,
    160, 162, 164, 167, 169, 171, 173, 175,

    177, 180, 182, 184, 186, 189, 191, 193,
    196, 198, 200, 203, 205, 208, 210, 213,

    215, 218, 220, 223, 225, 228, 231, 233,
    236, 239, 241, 244, 247, 249, 252, 255
};


// ============================================================
// Internal states
// ============================================================

/*
 * 逆相揺らぎ用の間欠カオス内部値。
 *
 * 3277 / 32768 ≒ 0.10
 */
static uint16_t chaosValue = 3277u;


/*
 * 全体輝度用の間欠カオス内部値。
 *
 * 10923 / 32768 ≒ 0.333
 *
 * 逆相側とは異なる値から開始することで
 * 同期しにくくする。
 */
static uint16_t globalChaosValue = 10923u;


/*
 * PWMの目標Duty。
 *
 * 10bit:
 *
 * 0 ～ 1023
 */
static uint16_t targetDuty5 = 0u;
static uint16_t targetDuty6 = 0u;


/*
 * 擬似乱数内部状態
 */
static uint16_t rngState = RNG_SEED;


/*
 * Timer2 interrupt:
 *
 * 1 tick ≒ 16.384ms
 */
static uint8_t updateTicks = 1u;
static uint8_t globalUpdateTicks = 1u;


/*
 * 起動フェード用。
 *
 * 約128tick = 約2.1秒
 */
static uint8_t fadeTicks = 0u;


// ============================================================
// Pseudo random generator
// ============================================================

static uint16_t rand16(void)
{
    uint16_t x = rngState;

    x ^= (uint16_t)(x << 7);
    x ^= (uint16_t)(x >> 9);
    x ^= (uint16_t)(x << 8);


    /*
     * xorshiftは0になると永久に0なので回避する。
     */
    if (x == 0u) {
        x = 0xA531u;
    }


    rngState = x;

    return x;
}


// ============================================================
// PPS control
// ============================================================

static void unlockPPS(void)
{
    INTCONbits.GIE = 0;

    PPSLOCK = 0x55;
    PPSLOCK = 0xAA;

    PPSLOCKbits.PPSLOCKED = 0;
}


static void lockPPS(void)
{
    PPSLOCK = 0x55;
    PPSLOCK = 0xAA;

    PPSLOCKbits.PPSLOCKED = 1;
}


// ============================================================
// PWM register access
// ============================================================

static void writePWM5(uint16_t duty10)
{
    /*
     * 念のため10bit最大値で飽和させる。
     */
    if (duty10 > 1023u) {
        duty10 = 1023u;
    }


    PWM5DCH = (uint8_t)(duty10 >> 2);

    PWM5DCL =
        (uint8_t)(
            (duty10 & 0x03u)
            <<
            6
        );
}


static void writePWM6(uint16_t duty10)
{
    /*
     * 念のため10bit最大値で飽和させる。
     */
    if (duty10 > 1023u) {
        duty10 = 1023u;
    }


    PWM6DCH = (uint8_t)(duty10 >> 2);

    PWM6DCL =
        (uint8_t)(
            (duty10 & 0x03u)
            <<
            6
        );
}


// ============================================================
// Brightness conversion
// ============================================================

/*
 * 8bit:
 *
 *   0 ～ 255
 *
 * を
 *
 * 10bit:
 *
 *   0 ～ 1023
 *
 * へ変換する。
 *
 * 255 -> 1023
 *
 * なのでPWM Duty 100%となる。
 */
static uint16_t brightnessToDuty10(uint8_t brightness)
{
    return
        (
            (uint16_t)brightness
            <<
            2
        )
        |
        (
            (uint16_t)brightness
            >>
            6
        );
}


/*
 * ------------------------------------------------------------
 * OUTPUT_GAIN
 * ------------------------------------------------------------
 *
 * gamma補正後の輝度を約1.098倍する。
 *
 * uint32_tを使用することで
 *
 *   255 * 281 = 71655
 *
 * の計算でもオーバーフローしない。
 *
 * 255を超えた場合は255へ飽和する。
 *
 * これにより一部のピークではPWMが100%に張り付く。
 */
static uint8_t applyOutputGain(uint8_t brightness)
{
    uint32_t temp;


    temp =
        (uint32_t)brightness
        *
        (uint32_t)OUTPUT_GAIN_Q8;


    /*
     * Q8なので256で除算。
     */
    temp >>= 8;


    /*
     * 8bit最大輝度へ飽和。
     *
     * ここで255を超えないことを保証する。
     */
    if (temp > 255UL) {
        temp = 255UL;
    }


    return (uint8_t)temp;
}


/*
 * MASTER_BRIGHTNESSを適用する。
 *
 * OUTPUT_GAIN適用後は0～255であることが
 * applyOutputGain()によって保証される。
 *
 * MASTER_BRIGHTNESS_Q8 <= 256で使用する限り、
 *
 * 最大:
 *
 *   255 * 256
 *   = 65280
 *
 * なのでuint16_tに収まる。
 */
static uint8_t applyMasterBrightness(uint8_t brightness)
{
    uint16_t temp;


    temp =
        (uint16_t)brightness
        *
        (uint16_t)MASTER_BRIGHTNESS_Q8;


    return
        (uint8_t)(
            temp
            >>
            8
        );
}


// ============================================================
// Brightness calculation
// ============================================================

static void calculateBrightness(void)
{
    uint16_t delta;

    uint16_t globalRange;
    uint16_t globalBrightnessQ8;

    uint8_t raw5;
    uint8_t raw6;

    uint8_t gamma5;
    uint8_t gamma6;


    /*
     * ========================================================
     * Chaos 1
     *
     * LED1 / LED2の逆相揺らぎ
     * ========================================================
     */

    /*
     * chaosValue:
     *
     * 0 ～ 32768
     *
     * を
     *
     * 0 ～ FLICKER_DEPTH
     *
     * に変換する。
     */
    delta =
        (uint16_t)(
            (
                (uint32_t)chaosValue
                *
                (uint32_t)FLICKER_DEPTH
            )
            >>
            15
        );


    /*
     * CH1 / CH2を逆方向へ変化させる。
     *
     * FLICKER_DEPTH = 50:
     *
     * CH1 = 205 ～ 255
     * CH2 = 255 ～ 205
     */
    raw5 =
        (uint8_t)(
            255u
            -
            FLICKER_DEPTH
            +
            delta
        );


    raw6 =
        (uint8_t)(
            255u
            -
            delta
        );


    /*
     * ========================================================
     * Chaos 2
     *
     * ランタン全体の明暗揺らぎ
     * ========================================================
     */

    globalRange =
        (uint16_t)(
            GLOBAL_BRIGHTNESS_MAX_Q8
            -
            GLOBAL_BRIGHTNESS_MIN_Q8
        );


    /*
     * 32bitで乗算。
     *
     * globalChaosValue <= 32768
     * globalRange      = 76
     *
     * 最大でも約2.5Mなのでuint32_tには十分収まる。
     */
    globalBrightnessQ8 =
        (uint16_t)(
            GLOBAL_BRIGHTNESS_MIN_Q8
            +
            (
                (
                    (uint32_t)globalChaosValue
                    *
                    (uint32_t)globalRange
                )
                >>
                15
            )
        );


    /*
     * CH1 / CH2へ同じ倍率を掛けることで、
     * 逆相関係を維持したまま
     * ランタン全体を明暗させる。
     *
     * 最大:
     *
     *   255 * 256
     *   = 65280
     *
     * だが、計算は安全側でuint32_tを使用。
     */
    raw5 =
        (uint8_t)(
            (
                (uint32_t)raw5
                *
                (uint32_t)globalBrightnessQ8
            )
            >>
            8
        );


    raw6 =
        (uint8_t)(
            (
                (uint32_t)raw6
                *
                (uint32_t)globalBrightnessQ8
            )
            >>
            8
        );


    /*
     * gamma補正
     */
    gamma5 = gamma8[raw5];
    gamma6 = gamma8[raw6];


    /*
     * --------------------------------------------------------
     * 出力ゲイン
     * --------------------------------------------------------
     *
     * 約1.098倍する。
     *
     * 255を超える場合はapplyOutputGain()内部で
     * 255へ飽和する。
     *
     * したがってPWM100%を超えることはない。
     */
    gamma5 = applyOutputGain(gamma5);
    gamma6 = applyOutputGain(gamma6);


    /*
     * MASTER_BRIGHTNESS
     *
     * 現在は256 = 100%
     */
    gamma5 = applyMasterBrightness(gamma5);
    gamma6 = applyMasterBrightness(gamma6);


    /*
     * 8bit -> 10bit PWM
     */
    targetDuty5 = brightnessToDuty10(gamma5);
    targetDuty6 = brightnessToDuty10(gamma6);
}


// ============================================================
// Intermittent chaos
// ============================================================

static void advanceChaos(uint16_t *value)
{
    uint32_t square;
    uint16_t x;


    x = *value;


    if (x < Q15_HALF) {

        /*
         * value += 2 * value^2
         *
         * Q15:
         *
         * 2*x*x / 32768
         *
         * =
         *
         * x*x / 16384
         */
        square =
            (uint32_t)x
            *
            (uint32_t)x;


        x +=
            (uint16_t)(
                square
                >>
                14
            );
    }
    else {

        uint16_t t;


        /*
         * t = 1 - value
         */
        t =
            (uint16_t)(
                Q15_ONE
                -
                x
            );


        square =
            (uint32_t)t
            *
            (uint32_t)t;


        x -=
            (uint16_t)(
                square
                >>
                14
            );
    }


    /*
     * 0または1付近まで来た場合、
     * 乱数を再注入する。
     */
    if (
        (x <= CHAOS_THRESHOLD)
        ||
        (
            x
            >=
            (uint16_t)(
                Q15_ONE
                -
                CHAOS_THRESHOLD
            )
        )
    ) {

        uint16_t range;


        range =
            (uint16_t)(
                Q15_ONE
                -
                (2UL * CHAOS_THRESHOLD)
            );


        /*
         * rand16()最大65535
         *
         * range ≒ 28508
         *
         * 積は約1.87e9なのでuint32_tに収まる。
         */
        x =
            CHAOS_THRESHOLD
            +
            (uint16_t)(
                (
                    (uint32_t)rand16()
                    *
                    (uint32_t)range
                )
                >>
                16
            );
    }


    *value = x;
}


// ============================================================
// Flicker timing
// ============================================================

/*
 * ------------------------------------------------------------
 * Chaos 1
 *
 * LED1 / LED2逆相揺らぎ
 * ------------------------------------------------------------
 *
 * 基本:
 *
 * 5 ～ 8 tick
 *
 * 約82 ～ 131ms
 *
 * 約1/3の確率で
 *
 * +3 ～ +10 tick
 *
 * を追加する。
 */
static uint8_t getNextUpdateTicks(void)
{
    uint8_t ticks;
    uint16_t r;


    r = rand16();


    ticks =
        (uint8_t)(
            5u
            +
            (r & 0x03u)
        );


    if (
        (uint8_t)(
            rand16()
            &
            0xFFu
        )
        <
        85u
    ) {

        ticks +=
            (uint8_t)(
                3u
                +
                (rand16() & 0x07u)
            );
    }


    return ticks;
}


/*
 * ------------------------------------------------------------
 * Chaos 2
 *
 * ランタン全体の明暗揺らぎ
 * ------------------------------------------------------------
 *
 * 基本:
 *
 * 4 ～ 7 tick
 *
 * 約66 ～ 115ms
 *
 * 約1/4の確率で
 *
 * +2 ～ +5 tick
 *
 * 約33 ～ 82ms
 *
 * を追加する。
 *
 * 最大:
 *
 * 12 tick
 *
 * 約197ms
 */
static uint8_t getNextGlobalUpdateTicks(void)
{
    uint8_t ticks;
    uint16_t r;


    r = rand16();


    ticks =
        (uint8_t)(
            4u
            +
            (r & 0x03u)
        );


    if (
        (uint8_t)(
            rand16()
            &
            0xFFu
        )
        <
        64u
    ) {

        ticks +=
            (uint8_t)(
                2u
                +
                (rand16() & 0x03u)
            );
    }


    return ticks;
}


// ============================================================
// Peripheral Module Disable
// ============================================================

static void disableUnusedPeripherals(void)
{
    /*
     * Fixed Voltage Reference
     * Clock Reference
     * Interrupt On Change
     */
    PMD0bits.FVRMD  = 1;
    PMD0bits.CLKRMD = 1;
    PMD0bits.IOCMD  = 1;


    /*
     * Timer2のみ使用。
     */
    PMD1bits.NCOMD  = 1;
    PMD1bits.TMR0MD = 1;
    PMD1bits.TMR1MD = 1;
    PMD1bits.TMR2MD = 0;


    /*
     * Analog peripherals停止。
     */
    PMD2bits.DACMD  = 1;
    PMD2bits.ADCMD  = 1;
    PMD2bits.CMP1MD = 1;


    /*
     * PWM5 / PWM6のみ使用。
     */
    PMD3bits.CWG1MD = 1;

    PMD3bits.PWM5MD = 0;
    PMD3bits.PWM6MD = 0;

    PMD3bits.CCP1MD = 1;
    PMD3bits.CCP2MD = 1;


    /*
     * UART / SPI / I2C停止。
     */
    PMD4bits.UART1MD = 1;
    PMD4bits.MSSP1MD = 1;


    /*
     * CLC / DSM停止。
     */
    PMD5bits.CLC1MD = 1;
    PMD5bits.CLC2MD = 1;
    PMD5bits.DSMMD  = 1;
}


// ============================================================
// GPIO initialization
// ============================================================

static void initGPIO(void)
{
    /*
     * PORTAをすべてデジタル。
     */
    ANSELA = 0x00;


    /*
     * Open Drain無効。
     */
    ODCONA = 0x00;


    /*
     * Weak Pull-up無効。
     */
    WPUA = 0x00;


    /*
     * 出力LatchをLOW。
     */
    LATA = 0x00;


    /*
     * PWM初期化中はRA4 / RA5を入力にする。
     *
     * RA3 : MCLR
     * RA4 : input temporarily
     * RA5 : input temporarily
     *
     * RA0 / RA1 / RA2 : LOW output
     */
    TRISA = 0x38;
}


// ============================================================
// PWM initialization
// ============================================================

static void initPWM(void)
{
    /*
     * PWM5 / PWM6停止。
     */
    PWM5CON = 0x00;
    PWM6CON = 0x00;


    /*
     * 初期Duty = 0
     */
    writePWM5(0u);
    writePWM6(0u);


    /*
     * ========================================================
     * Timer2
     * ========================================================
     *
     * FOSC = 1MHz
     *
     * PWM frequency =
     *
     * FOSC /
     * [4 * (PR2 + 1) * prescaler]
     *
     * PR2 = 255
     * prescaler = 1
     *
     * =>
     *
     * 約976.56Hz
     */


    PR2  = 255u;
    TMR2 = 0u;


    PIR1bits.TMR2IF = 0;


    /*
     * T2OUTPS = 1:16
     * TMR2ON  = 1
     * T2CKPS  = 1:1
     */
    T2CON = 0x7C;


    /*
     * 最初のTimer2周期完了を待つ。
     */
    while (PIR1bits.TMR2IF == 0) {
        ;
    }


    PIR1bits.TMR2IF = 0;


    /*
     * ========================================================
     * PPS
     * ========================================================
     *
     * 0x02 = PWM5
     * 0x03 = PWM6
     *
     * RA4 <- PWM5
     * RA5 <- PWM6
     */
    unlockPPS();

    RA4PPS = 0x02;
    RA5PPS = 0x03;

    lockPPS();


    /*
     * PWM enable
     */
    PWM5CONbits.PWM5EN = 1;
    PWM6CONbits.PWM6EN = 1;


    /*
     * RA4 / RA5を出力へ変更。
     */
    TRISAbits.TRISA4 = 0;
    TRISAbits.TRISA5 = 0;


    /*
     * Timer2 interruptを
     * Idle解除用として使用する。
     */
    PIR1bits.TMR2IF = 0;
    PIE1bits.TMR2IE = 1;


    INTCONbits.PEIE = 1;


    /*
     * GIEは0。
     *
     * Timer2 interrupt requestによって
     * Idleから復帰するがISRへは飛ばない。
     */
    INTCONbits.GIE = 0;
}


// ============================================================
// Idle mode initialization
// ============================================================

static void initIdle(void)
{
    /*
     * SLEEP時にFull SleepではなくIdleへ移行する。
     *
     * CPUは停止するが、
     * Timer2 / PWMは動作を続ける。
     */
    CPUDOZEbits.IDLEN = 1;
}


// ============================================================
// Main
// ============================================================

void main(void)
{
    uint8_t brightnessChanged;


    /*
     * GPIO初期化
     */
    initGPIO();


    /*
     * 未使用周辺回路停止
     */
    disableUnusedPeripherals();


    /*
     * 初期輝度計算
     */
    calculateBrightness();


    /*
     * PWM初期化
     */
    initPWM();


    /*
     * 逆相カオスの次回更新時間
     */
    updateTicks =
        getNextUpdateTicks();


    /*
     * 全体カオスの次回更新時間
     */
    globalUpdateTicks =
        getNextGlobalUpdateTicks();


    /*
     * Idle有効
     */
    initIdle();


    /*
     * ========================================================
     * Main loop
     * ========================================================
     */

    while (1) {

        /*
         * CPUをIdleへ。
         *
         * 約16.4ms後にTimer2で復帰する。
         */
        SLEEP();
        NOP();


        /*
         * Timer2 interrupt flag clear
         */
        if (PIR1bits.TMR2IF) {
            PIR1bits.TMR2IF = 0;
        }


        brightnessChanged = 0u;


        /*
         * ====================================================
         * Chaos 1
         *
         * LED1 / LED2逆相揺らぎ
         * ====================================================
         */

        if (updateTicks > 0u) {
            updateTicks--;
        }


        if (updateTicks == 0u) {

            advanceChaos(&chaosValue);


            updateTicks =
                getNextUpdateTicks();


            brightnessChanged = 1u;
        }


        /*
         * ====================================================
         * Chaos 2
         *
         * ランタン全体の明暗揺らぎ
         * ====================================================
         */

        if (globalUpdateTicks > 0u) {
            globalUpdateTicks--;
        }


        if (globalUpdateTicks == 0u) {

            advanceChaos(&globalChaosValue);


            globalUpdateTicks =
                getNextGlobalUpdateTicks();


            brightnessChanged = 1u;
        }


        /*
         * どちらかのカオスが更新された場合、
         * 新しいPWM目標値を計算する。
         */
        if (brightnessChanged) {
            calculateBrightness();
        }


        /*
         * ====================================================
         * 起動フェードイン
         * ====================================================
         *
         * 約2.1秒で
         *
         * 0% -> 通常輝度
         *
         * へ変化する。
         */

        if (fadeTicks < 128u) {

            uint16_t out5;
            uint16_t out6;


            fadeTicks++;


            /*
             * targetDuty:
             *
             * 最大1023
             *
             * fadeTicks:
             *
             * 最大128
             *
             * 最大積:
             *
             * 1023 * 128
             * = 130944
             *
             * uint16_tを超えるため、
             * 必ずuint32_tで乗算する。
             */
            out5 =
                (uint16_t)(
                    (
                        (uint32_t)targetDuty5
                        *
                        (uint32_t)fadeTicks
                    )
                    >>
                    7
                );


            out6 =
                (uint16_t)(
                    (
                        (uint32_t)targetDuty6
                        *
                        (uint32_t)fadeTicks
                    )
                    >>
                    7
                );


            writePWM5(out5);
            writePWM6(out6);
        }
        else {

            /*
             * フェード終了後は、
             * 揺らぎが変化した場合のみPWMを書き換える。
             */
            if (brightnessChanged) {

                writePWM5(targetDuty5);
                writePWM6(targetDuty6);
            }
        }
    }
}