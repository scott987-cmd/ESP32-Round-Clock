#pragma once
#include <stdint.h>
#define CONTENT_BYTES 1971184u
#define CONTENT_CRC 3054325021u
#define CONTENT_ENGLISH_OFFSET 16u
#define CONTENT_ENGLISH_BYTES 423128u
typedef struct {const char *title,*text,*left,*right;int picture,a,b;uint32_t audio,bytes;} story_node_t;
static const story_node_t builtin_story[]={
{"小猫找星星","小猫发现窗外有一颗闪亮的星星。它想把这份美好分享给朋友。先去找谁一起出发呢？","去问小鸟","去问小鱼",0,1,2,423144,286498},
{"小鸟的邀请","小鸟说：星星在天上，不用摘下来。我们可以找一个开阔的地方，一起欣赏。小猫点点头。","一起看星星","回家画星星",2,3,4,709644,302102},
{"小鱼的秘密","小鱼指着水面说：看，星星也在湖里跳舞！那是星光的倒影。小猫知道了，水里的星星不用捞。","一起看星星","回家画星星",3,3,4,1011748,316220},
{"分享美好","朋友们坐在一起，看星星一闪一闪。小猫说：原来，美好的东西不一定要带走，和朋友分享也很快乐。","再听一遍","重新开始",0,-1,0,1327968,337772},
{"小小画家","小猫回到家，把今晚看到的星光画在纸上，送给朋友。大家开心地说：这是一份温暖的礼物！","再听一遍","重新开始",10,-1,0,1665740,305444}
};
