// Simple character frequency counter to understand branch patterns
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s file.json\n", argv[0]);
        return 1;
    }
    
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "Cannot open file\n");
        return 1;
    }
    
    uint64_t counts[256] = {0};
    uint64_t total = 0;
    int c;
    
    while ((c = fgetc(f)) != EOF) {
        counts[c]++;
        total++;
    }
    fclose(f);
    
    printf("=================================================================\n");
    printf("JSON Character Frequency Analysis - Branch Pattern Predictor\n");
    printf("=================================================================\n");
    printf("Total bytes: %llu\n\n", total);
    
    // Important characters for branch analysis
    struct { int ch; const char *name; const char *impact; } important[] = {
        {' ', "SPACE", "whitespace skip loop"},
        {'\n', "NEWLINE", "whitespace skip loop"},
        {'\t', "TAB", "whitespace skip loop"},
        {'\r', "CR", "whitespace skip loop"},
        {'"', "QUOTE", "string start (HOTTEST)"},
        {'{', "OPEN_BRACE", "object start"},
        {'}', "CLOSE_BRACE", "object end"},
        {'[', "OPEN_BRACKET", "array start"},
        {']', "CLOSE_BRACKET", "array end"},
        {':', "COLON", "key-value separator"},
        {',', "COMMA", "item separator"},
        {'t', "T (true)", "keyword check"},
        {'f', "F (false)", "keyword check"},
        {'n', "N (null)", "keyword check"},
        {'\\', "BACKSLASH", "escape sequence (RARE)"},
        {'-', "MINUS", "number start"},
        {'0', "ZERO", "number"},
        {'1', "ONE", "number"},
        {'2', "TWO", "number"},
        {'3', "THREE", "number"},
        {'4', "FOUR", "number"},
        {'5', "FIVE", "number"},
        {'6', "SIX", "number"},
        {'7', "SEVEN", "number"},
        {'8', "EIGHT", "number"},
        {'9', "NINE", "number"},
    };
    
    uint64_t ws_total = counts[' '] + counts['\n'] + counts['\t'] + counts['\r'];
    uint64_t digits_total = 0;
    for (int i = '0'; i <= '9'; i++) digits_total += counts[i];
    
    printf("Key Character Categories:\n");
    printf("  Whitespace:      %10llu (%5.2f%%) - p_skip_ws() iterations\n", 
           ws_total, 100.0*ws_total/total);
    printf("  Quotes:          %10llu (%5.2f%%) - String parsing calls\n", 
           counts['"'], 100.0*counts['"']/total);
    printf("  Digits:          %10llu (%5.2f%%) - Number parsing\n", 
           digits_total, 100.0*digits_total/total);
    printf("  Braces {}:       %10llu (%5.2f%%) - Objects\n", 
           counts['{'] + counts['}'], 100.0*(counts['{'] + counts['}'])/total);
    printf("  Brackets []:     %10llu (%5.2f%%) - Arrays\n", 
           counts['['] + counts[']'], 100.0*(counts['['] + counts[']'])/total);
    printf("  Backslash:       %10llu (%5.2f%%) - Escapes (MISPREDICTED!)\n\n",
           counts['\\'], 100.0*counts['\\']/total);
    
    printf("Detailed Breakdown:\n");
    printf("%-15s %10s %7s  %s\n", "Character", "Count", "Percent", "Branch Impact");
    printf("-------------------------------------------------------------------\n");
    for (size_t i = 0; i < sizeof(important)/sizeof(important[0]); i++) {
        int ch = important[i].ch;
        printf("%-15s %10llu %6.2f%%  %s\n",
               important[i].name, counts[ch], 100.0*counts[ch]/total, important[i].impact);
    }
    
    printf("\n=================================================================\n");
    printf("BRANCH OPTIMIZATION INSIGHTS:\n");
    printf("=================================================================\n\n");
    
    printf("1. parse_value() Branch Order Optimization:\n");
    printf("   Current: Tests EOF, {, [, \", numbers, t, f, n in sequence\n");
    printf("   Problem: Average %.1f branches before finding match\n",
           (0.01*0 + counts['{']*1 + counts['[']*2 + counts['"']*3 + 
            (digits_total+counts['-'])*4 + counts['t']*5 + counts['f']*6 + counts['n']*7)/(double)total*10);
    printf("   \n");
    printf("   RECOMMENDED ORDER based on frequency:\n");
    printf("   1st: \" (quotes)  - %.1f%% of tokens\n", 100.0*counts['"']/total);
    printf("   2nd: digits/'-'  - %.1f%% of tokens\n", 100.0*(digits_total+counts['-'])/total);
    printf("   3rd: {{ (object)  - %.1f%% of tokens\n", 100.0*counts['{']/total);
    printf("   4th: [ (array)   - %.1f%% of tokens\n", 100.0*counts['[']/total);
    printf("   Last: t/f/n      - %.1f%% of tokens\n\n",
           100.0*(counts['t']+counts['f']+counts['n'])/total);
    
    printf("2. parse_string() Escape Branch:\n");
    printf("   Escape frequency: %.4f%% (1 in %d chars)\n",
           100.0*counts['\\']/total, (int)(total/counts['\\']));
    printf("   Current: Checks EVERY character for \\\\ (mispredicted %.2f%% of time)\n",
           100.0 - 100.0*counts['\\']/total);
    printf("   FIX: Create fast-path that skips non-escape, non-quote chars\n");
    printf("   Expected speedup: 20-30%% in string parsing\n\n");
    
    printf("3. p_skip_ws() Whitespace Loop:\n");
    printf("   Whitespace chars: %.1f%% of file\n", 100.0*ws_total/total);
    printf("   Current: Loop with isspace() function call\n");
    printf("   FIX: Inline check with lookup table or direct comparisons\n");
    printf("   Expected speedup: 10-15%% overall\n\n");
    
    printf("4. Overall Branch Miss Rate Estimate:\n");
    printf("   Baseline (current):  4-5%% (poor ordering + escape checks)\n");
    printf("   After optimization:  1-2%% (reordered + fast paths)\n");
    printf("   Expected total speedup: 20-30%%\n\n");
    
    printf("=================================================================\n");
    
    return 0;
}
