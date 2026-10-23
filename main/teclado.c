#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "audio.h"
#include "buttons.h"
#include "display.h"

char letters[] = "abcdefghijklmnopqrstuvwxyz";
char caps[] =    "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
char symb[] =    "!\"#$\%&'()*+,-./:;<=>?@[]\\^_`{|}~";
char numb[] =    "0123456789";

char* readline(){
    int ptr = 0;
    int size = 64;
    res = malloc(size);

    while (true){
        if ptr>=size {
            realloc(res, size*2);
            size *= 2;
        }
        res[ptr] = read_char();
        ptr++;
    }
    return res;
}

char read_char(){
    while (true){


        vTaskDelay(pdMS_TO_TICKS(1000));

    }
}


render 4 diagonal quadrants, occupying most of the screen except for a bar on the side with most recent inputs. button 1 switches between 4 charsets. 2 is space. 3 is delete. 4 is accept. we do a quaternary search, using the joystick to select one fo the quadrants that renders a quarter of the charset. then those chars are rendered in quadrants. 