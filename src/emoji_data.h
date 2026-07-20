/* Emoji picker data for the built-in `wispctl emoji` menu.
 * Generated once from github/gemoji (db/emoji.json), curated to the 256-entry
 * menu cap: category order (smileys → people → animals → food → activities →
 * travel → objects → symbols), flags and skin-tone variants dropped. Each row
 * is { glyph, "description + aliases + tags" } — the tail is search fodder.
 * Regenerate by re-running the gemoji fetch/select; do not hand-tune rows. */
#ifndef WISP_EMOJI_DATA_H
#define WISP_EMOJI_DATA_H

#define EMOJI_INIT { \
    { "😀", "grinning face smile happy" }, \
    { "😃", "grinning face with big eyes smiley happy joy haha" }, \
    { "😄", "grinning face with smiling eyes smile happy joy laugh pleased" }, \
    { "😁", "beaming face with smiling eyes grin" }, \
    { "😆", "grinning squinting face laughing satisfied happy haha" }, \
    { "😅", "grinning face with sweat smile hot" }, \
    { "🤣", "rolling on the floor laughing rofl lol" }, \
    { "😂", "face with tears of joy" }, \
    { "🙂", "slightly smiling face" }, \
    { "🙃", "upside-down face upside down" }, \
    { "🫠", "melting face sarcasm dread" }, \
    { "😉", "winking face wink flirt" }, \
    { "😊", "smiling face with eyes blush proud" }, \
    { "😇", "smiling face with halo innocent angel" }, \
    { "🥰", "smiling face with hearts three love" }, \
    { "😍", "smiling face with heart-eyes heart eyes love crush" }, \
    { "🤩", "star-struck star struck eyes" }, \
    { "😘", "face blowing a kiss kissing heart flirt" }, \
    { "😗", "kissing face" }, \
    { "☺️", "smiling face relaxed blush pleased" }, \
    { "😚", "kissing face with closed eyes" }, \
    { "😙", "kissing face with smiling eyes" }, \
    { "🥲", "smiling face with tear" }, \
    { "😋", "face savoring food yum tongue lick" }, \
    { "😛", "face with tongue stuck out" }, \
    { "😜", "winking face with tongue stuck out eye prank silly" }, \
    { "🤪", "zany face goofy wacky" }, \
    { "😝", "squinting face with tongue stuck out closed eyes prank" }, \
    { "🤑", "money-mouth face money mouth rich" }, \
    { "🤗", "smiling face with open hands hugs" }, \
    { "🤭", "face with hand over mouth quiet whoops" }, \
    { "🫢", "face with open eyes and hand over mouth gasp shock" }, \
    { "🫣", "face with peeking eye" }, \
    { "🤫", "shushing face silence quiet" }, \
    { "🤔", "thinking face" }, \
    { "🫡", "saluting face respect" }, \
    { "🤐", "zipper-mouth face zipper mouth silence hush" }, \
    { "🤨", "face with raised eyebrow suspicious" }, \
    { "😐", "neutral face meh" }, \
    { "😑", "expressionless face" }, \
    { "😶", "face without mouth no mute silence" }, \
    { "🫥", "dotted line face invisible" }, \
    { "😶‍🌫️", "face in clouds" }, \
    { "😏", "smirking face smirk smug" }, \
    { "😒", "unamused face meh" }, \
    { "🙄", "face with rolling eyes roll" }, \
    { "😬", "grimacing face" }, \
    { "😮‍💨", "face exhaling" }, \
    { "🤥", "lying face liar" }, \
    { "🫨", "shaking face shock" }, \
    { "😌", "relieved face whew" }, \
    { "😔", "pensive face" }, \
    { "😪", "sleepy face tired" }, \
    { "🤤", "drooling face" }, \
    { "😴", "sleeping face zzz" }, \
    { "😷", "face with medical mask sick ill" }, \
    { "🤒", "face with thermometer sick" }, \
    { "🤕", "face with head-bandage head bandage hurt" }, \
    { "🤢", "nauseated face sick barf disgusted" }, \
    { "🤮", "face vomiting barf sick" }, \
    { "🤧", "sneezing face achoo sick" }, \
    { "🥵", "hot face heat sweating" }, \
    { "🥶", "cold face freezing ice" }, \
    { "🥴", "woozy face groggy" }, \
    { "😵", "face with crossed-out eyes dizzy" }, \
    { "😵‍💫", "face with spiral eyes" }, \
    { "🤯", "exploding head mind blown" }, \
    { "🤠", "cowboy hat face" }, \
    { "🥳", "partying face celebration birthday" }, \
    { "🥸", "disguised face" }, \
    { "😎", "smiling face with sunglasses cool" }, \
    { "🤓", "nerd face geek glasses" }, \
    { "🧐", "face with monocle" }, \
    { "😕", "confused face" }, \
    { "🫤", "face with diagonal mouth confused" }, \
    { "😟", "worried face nervous" }, \
    { "🙁", "slightly frowning face" }, \
    { "☹️", "frowning face" }, \
    { "😮", "face with open mouth surprise impressed wow" }, \
    { "😯", "hushed face silence speechless" }, \
    { "😲", "astonished face amazed gasp" }, \
    { "😳", "flushed face" }, \
    { "🥺", "pleading face puppy eyes" }, \
    { "🥹", "face holding back tears gratitude" }, \
    { "😦", "frowning face with open mouth" }, \
    { "😧", "anguished face stunned" }, \
    { "😨", "fearful face scared shocked oops" }, \
    { "😰", "anxious face with sweat cold nervous" }, \
    { "😥", "sad but relieved face disappointed phew sweat nervous" }, \
    { "😢", "crying face cry sad tear" }, \
    { "😭", "loudly crying face sob sad cry bawling" }, \
    { "😱", "face screaming in fear scream horror shocked" }, \
    { "😖", "confounded face" }, \
    { "😣", "persevering face persevere struggling" }, \
    { "😞", "disappointed face sad" }, \
    { "😓", "downcast face with sweat" }, \
    { "😩", "weary face tired" }, \
    { "😫", "tired face upset whine" }, \
    { "🥱", "yawning face" }, \
    { "😤", "face with steam from nose triumph smug" }, \
    { "😡", "enraged face rage pout angry" }, \
    { "😠", "angry face mad annoyed" }, \
    { "🤬", "face with symbols on mouth cursing foul" }, \
    { "😈", "smiling face with horns imp devil evil" }, \
    { "👿", "angry face with horns imp devil evil" }, \
    { "💀", "skull dead danger poison" }, \
    { "☠️", "skull and crossbones danger pirate" }, \
    { "💩", "pile of poo hankey poop shit crap" }, \
    { "🤡", "clown face" }, \
    { "👹", "ogre japanese monster" }, \
    { "👺", "goblin japanese" }, \
    { "👻", "ghost halloween" }, \
    { "👽", "alien ufo" }, \
    { "👾", "alien monster space invader game retro" }, \
    { "🤖", "robot" }, \
    { "😺", "grinning cat smiley" }, \
    { "😸", "grinning cat with smiling eyes smile" }, \
    { "😹", "cat with tears of joy" }, \
    { "😻", "smiling cat with heart-eyes heart eyes" }, \
    { "😼", "cat with wry smile smirk" }, \
    { "😽", "kissing cat" }, \
    { "🙀", "weary cat scream horror" }, \
    { "😿", "crying cat face sad tear" }, \
    { "😾", "pouting cat" }, \
    { "🙈", "see-no-evil monkey see no evil blind ignore" }, \
    { "🙉", "hear-no-evil monkey hear no evil deaf" }, \
    { "🙊", "speak-no-evil monkey speak no evil mute hush" }, \
    { "💌", "love letter email envelope" }, \
    { "💘", "heart with arrow cupid love" }, \
    { "💝", "heart with ribbon gift chocolates" }, \
    { "💖", "sparkling heart" }, \
    { "💗", "growing heart heartpulse" }, \
    { "💓", "beating heart heartbeat" }, \
    { "💞", "revolving hearts" }, \
    { "💕", "two hearts" }, \
    { "💟", "heart decoration" }, \
    { "❣️", "heart exclamation heavy" }, \
    { "💔", "broken heart" }, \
    { "❤️‍🔥", "heart on fire" }, \
    { "❤️‍🩹", "mending heart" }, \
    { "❤️", "red heart love" }, \
    { "🩷", "pink heart" }, \
    { "🧡", "orange heart" }, \
    { "💛", "yellow heart" }, \
    { "💚", "green heart" }, \
    { "💙", "blue heart" }, \
    { "🩵", "light blue heart" }, \
    { "💜", "purple heart" }, \
    { "🤎", "brown heart" }, \
    { "🖤", "black heart" }, \
    { "🩶", "grey heart" }, \
    { "🤍", "white heart" }, \
    { "💋", "kiss mark lipstick" }, \
    { "💯", "hundred points 100 score perfect" }, \
    { "💢", "anger symbol angry" }, \
    { "💥", "collision boom explode" }, \
    { "💫", "dizzy star" }, \
    { "💦", "sweat droplets drops water workout" }, \
    { "💨", "dashing away dash wind blow fast" }, \
    { "🕳️", "hole" }, \
    { "💬", "speech balloon comment" }, \
    { "👁️‍🗨️", "eye in speech bubble" }, \
    { "🗨️", "left speech bubble" }, \
    { "🗯️", "right anger bubble" }, \
    { "💭", "thought balloon thinking" }, \
    { "💤", "ZZZ sleeping" }, \
    { "👋", "waving hand wave goodbye" }, \
    { "🤚", "raised back of hand" }, \
    { "🖐️", "hand with fingers splayed raised" }, \
    { "✋", "raised hand highfive stop" }, \
    { "🖖", "vulcan salute prosper spock" }, \
    { "🫱", "rightwards hand" }, \
    { "🫲", "leftwards hand" }, \
    { "🫳", "palm down hand" }, \
    { "🫴", "palm up hand" }, \
    { "🫷", "leftwards pushing hand" }, \
    { "🫸", "rightwards pushing hand" }, \
    { "👌", "OK hand" }, \
    { "🤌", "pinched fingers" }, \
    { "🤏", "pinching hand" }, \
    { "✌️", "victory hand v peace" }, \
    { "🤞", "crossed fingers luck hopeful" }, \
    { "🫰", "hand with index finger and thumb crossed" }, \
    { "🤟", "love-you gesture love you" }, \
    { "🤘", "sign of the horns metal" }, \
    { "🤙", "call me hand" }, \
    { "👈", "backhand index pointing left point" }, \
    { "👉", "backhand index pointing right point" }, \
    { "👆", "backhand index pointing up point 2" }, \
    { "🖕", "middle finger fu" }, \
    { "👇", "backhand index pointing down point" }, \
    { "☝️", "index pointing up point" }, \
    { "🫵", "index pointing at the viewer" }, \
    { "👍", "thumbs up +1 thumbsup approve ok" }, \
    { "👎", "thumbs down -1 thumbsdown disapprove bury" }, \
    { "✊", "raised fist power" }, \
    { "👊", "oncoming fist facepunch punch attack" }, \
    { "🤛", "left-facing fist left" }, \
    { "🤜", "right-facing fist right" }, \
    { "👏", "clapping hands clap praise applause" }, \
    { "🙌", "raising hands raised hooray" }, \
    { "🫶", "heart hands love" }, \
    { "👐", "open hands" }, \
    { "🤲", "palms up together" }, \
    { "🤝", "handshake deal" }, \
    { "🙏", "folded hands pray please hope wish" }, \
    { "✍️", "writing hand" }, \
    { "💅", "nail polish care beauty manicure" }, \
    { "🤳", "selfie" }, \
    { "💪", "flexed biceps muscle flex bicep strong workout" }, \
    { "🦾", "mechanical arm" }, \
    { "🦿", "mechanical leg" }, \
    { "🦵", "leg" }, \
    { "🦶", "foot" }, \
    { "👂", "ear hear sound listen" }, \
    { "🦻", "ear with hearing aid" }, \
    { "👃", "nose smell" }, \
    { "🧠", "brain" }, \
    { "🫀", "anatomical heart" }, \
    { "🫁", "lungs" }, \
    { "🦷", "tooth" }, \
    { "🦴", "bone" }, \
    { "👀", "eyes look see watch" }, \
    { "👁️", "eye" }, \
    { "👅", "tongue taste" }, \
    { "👄", "mouth lips kiss" }, \
    { "🫦", "biting lip" }, \
    { "👶", "baby child newborn" }, \
    { "🧒", "child" }, \
    { "👦", "boy child" }, \
    { "👧", "girl child" }, \
    { "🧑", "person adult" }, \
    { "👱", "person: blond hair haired person" }, \
    { "👨", "man mustache father dad" }, \
    { "🧔", "person: beard bearded person" }, \
    { "🧔‍♂️", "man: beard man" }, \
    { "🧔‍♀️", "woman: beard woman" }, \
    { "👨‍🦰", "man: red hair haired man" }, \
    { "👨‍🦱", "man: curly hair haired man" }, \
    { "👨‍🦳", "man: white hair haired man" }, \
    { "👨‍🦲", "man: bald man" }, \
    { "👩", "woman girls" }, \
    { "👩‍🦰", "woman: red hair haired woman" }, \
    { "🧑‍🦰", "person: red hair person" }, \
    { "👩‍🦱", "woman: curly hair haired woman" }, \
    { "🧑‍🦱", "person: curly hair person" }, \
    { "👩‍🦳", "woman: white hair haired woman" }, \
    { "🧑‍🦳", "person: white hair person" }, \
    { "👩‍🦲", "woman: bald woman" }, \
    { "🧑‍🦲", "person: bald person" }, \
    { "👱‍♀️", "woman: blond hair haired woman blonde" }, \
    { "👱‍♂️", "man: blond hair haired man" }, \
    { "🧓", "older person adult" }, \
    { "👴", "old man older" }, \
    { "👵", "old woman older" }, \
    { "🙍", "person frowning" }, \
}

#endif
