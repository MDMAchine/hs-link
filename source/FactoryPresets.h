#pragma once
// Compiled-in factory presets (read-only). Names carry the "Factory/" prefix and
// are listed before user presets. Only GenSettings fields are used. ASCII only.
#include "PluginProcessor.h"

namespace FactoryPresets
{
struct Entry
{
    const char* name;       // "Factory/<Category>/<Title>"
    const char* caption;
    double bpm;
    float duration;
    int keyIndex;           // 0 = auto, else 1..kNumKeys
    bool instrumental;
    int takes = 4;          // 1..8
    int modeId = 1;         // 1 text2music, 2 repaint, 3 lego
};

inline constexpr const char* kPrefix = "Factory/";

inline const Entry kEntries[] = {
    { "Factory/EDM/Night Drive",        "Driving synthwave EDM, pulsing bass, bright arps, wide pads, four on the floor", 124.0, 60.0f, 0, true },
    { "Factory/EDM/Festival Drop",      "Big room EDM, huge supersaw lead, punchy kick, build and drop, euphoric", 128.0, 60.0f, 0, true },
    { "Factory/EDM/Deep House Sunset",  "Deep house, warm rolling bassline, soft chords, shuffled hats, relaxed groove", 122.0, 60.0f, 0, true },
    { "Factory/Hip-Hop/Boom Bap Cipher", "Boom bap hip-hop beat, dusty drums, jazzy sample chops, upright bass", 90.0, 45.0f, 0, true },
    { "Factory/Hip-Hop/Trap Midnight",  "Dark trap beat, booming 808, fast hi-hat rolls, eerie bells", 140.0, 45.0f, 0, true },
    { "Factory/Hip-Hop/Soul Flip",      "Soulful hip-hop, warm vinyl samples, laid back drums, horns", 88.0, 45.0f, 0, true },
    { "Factory/Ambient/Glacier",        "Slow evolving ambient drones, shimmering textures, distant bells, calm", 70.0, 90.0f, 0, true },
    { "Factory/Ambient/Forest Morning", "Gentle ambient, soft piano, field recording textures, airy pads, peaceful", 80.0, 90.0f, 0, true },
    { "Factory/Ambient/Deep Space",     "Dark ambient space music, low rumbles, granular swells, vast and cold", 60.0, 90.0f, 0, true },
    { "Factory/Cinematic/Rising Hero",  "Epic orchestral trailer, soaring strings, brass fanfare, taiko drums, triumphant", 100.0, 60.0f, 0, true },
    { "Factory/Cinematic/Tension",      "Suspenseful cinematic underscore, pulsing strings, low synth hits, dark", 110.0, 60.0f, 0, true },
    { "Factory/Cinematic/Fragile Hope", "Emotional cinematic piano and strings, slow build, bittersweet", 72.0, 60.0f, 0, true },
    { "Factory/Rock/Garage Anthem",     "Energetic garage rock, crunchy guitars, driving drums, raw and loud", 140.0, 45.0f, 0, true },
    { "Factory/Rock/Indie Drift",       "Dreamy indie rock, jangly clean guitars, steady drums, warm bass", 118.0, 45.0f, 0, true },
    { "Factory/Rock/Heavy Riff",        "Heavy rock, distorted riffs, pounding drums, powerful and dark", 105.0, 45.0f, 0, true },
    { "Factory/Lo-Fi/Rainy Study",      "Lo-fi hip-hop, mellow Rhodes chords, vinyl crackle, soft drums, rain ambience", 80.0, 60.0f, 0, true },
    { "Factory/Lo-Fi/Late Night Tape",  "Lo-fi chill beat, tape wobble, muted guitar, sleepy and cozy", 75.0, 60.0f, 0, true },
    { "Factory/Jazz/Smoky Lounge",      "Smooth jazz trio, brushed drums, walking upright bass, piano, late night", 100.0, 60.0f, 0, true },
    { "Factory/Jazz/Bebop Rush",        "Fast bebop jazz, saxophone lead, ride cymbal swing, lively piano comping", 200.0, 45.0f, 0, true },
    { "Factory/Pop/Summer Hook",        "Upbeat pop, bright synths, catchy melody, punchy drums, feel good", 115.0, 45.0f, 0, true },
    { "Factory/Pop/Midnight Ballad",    "Emotional pop ballad, piano and soft strings, intimate and warm", 76.0, 45.0f, 0, true },
    { "Factory/World/Desert Caravan",   "Middle eastern inspired groove, oud, hand percussion, hypnotic", 96.0, 60.0f, 0, true },
    { "Factory/World/Afrobeat Sun",     "Afrobeat groove, syncopated percussion, bright guitars, horns, joyful", 108.0, 60.0f, 0, true },
    { "Factory/World/Island Breeze",    "Relaxed reggae, offbeat guitar skank, deep bass, warm and sunny", 82.0, 60.0f, 0, true },
    { "Factory/Funk/Disco Glow",        "Funky disco groove, slap bass, rhythm guitar, strings, danceable", 118.0, 45.0f, 0, true },
    { "Factory/Drum and Bass/Liquid Rain", "Liquid drum and bass, rolling breakbeats, deep sub bass, lush pads, soulful and flowing", 174.0, 60.0f, 0, true, 8, 1 },
    { "Factory/Drum and Bass/Neuro Grind", "Neurofunk drum and bass, gritty reese bass, tight snares, dark and mechanical", 172.0, 45.0f, 0, true, 6, 1 },
    { "Factory/Techno/Warehouse Pulse", "Hypnotic warehouse techno, relentless kick, rumbling low end, metallic percussion", 130.0, 90.0f, 0, true, 4, 1 },
    { "Factory/Techno/Minimal Clicks", "Minimal techno, sparse clicks, subtle bass, evolving micro grooves, cool and restrained", 125.0, 120.0f, 0, true, 3, 1 },
    { "Factory/Classical/String Quartet", "Classical string quartet, expressive violin melody, cello counterpoint, elegant", 84.0, 90.0f, 0, true, 4, 1 },
    { "Factory/Classical/Solo Nocturne", "Romantic solo piano nocturne, delicate rubato, gentle dynamics, intimate", 66.0, 120.0f, 0, true, 2, 1 },
    { "Factory/Country/Back Porch", "Acoustic country, fingerpicked guitar, fiddle, light brushed snare, warm and homey", 96.0, 45.0f, 0, true, 4, 1 },
    { "Factory/Country/Highway Twang", "Upbeat country rock, twangy telecaster, pedal steel, driving train beat", 150.0, 45.0f, 0, true, 5, 1 },
    { "Factory/Blues/Delta Slide", "Raw delta blues, slide guitar, stomping foot rhythm, gritty and soulful", 78.0, 60.0f, 0, true, 3, 1 },
    { "Factory/Blues/Chicago Shuffle", "Electric Chicago blues shuffle, harmonica, overdriven guitar, walking bass", 112.0, 60.0f, 0, true, 4, 1 },
    { "Factory/Metal/Thrash Storm", "Fast thrash metal, palm muted riffs, double kick drums, aggressive", 190.0, 45.0f, 0, true, 6, 1 },
    { "Factory/Metal/Doom Monolith", "Slow doom metal, crushing downtuned guitars, heavy drums, ominous and vast", 62.0, 90.0f, 0, true, 3, 1 },
    { "Factory/Latin/Salsa Fiesta", "Salsa, bright brass section, piano montuno, congas and timbales, festive", 180.0, 60.0f, 0, true, 4, 1 },
    { "Factory/Latin/Bossa Breeze", "Gentle bossa nova, nylon guitar, soft shaker, mellow and sunny", 132.0, 60.0f, 0, true, 4, 1 },
    { "Factory/Synthwave/Neon Chase", "Retro synthwave chase, gated drums, analog bass arps, 80s action energy", 150.0, 60.0f, 0, true, 6, 1 },
    { "Factory/Synthwave/Chrome Dusk", "Slow dreamy synthwave, warm analog pads, nostalgic lead, melancholy", 86.0, 90.0f, 0, true, 4, 1 },
    { "Factory/Orchestral/Pastoral Dawn", "Pastoral orchestral piece, woodwinds, gentle horns, flowing strings, hopeful", 92.0, 120.0f, 0, true, 3, 1 },
    { "Factory/Experimental/Glitch Garden", "Experimental glitch, chopped textures, granular stutters, quirky percussion", 100.0, 45.0f, 0, true, 8, 1 },
    { "Factory/Experimental/Tape Loops", "Experimental tape loop music, degraded loops, hiss, slowly shifting harmony, haunting", 58.0, 180.0f, 0, true, 2, 1 },
    { "Factory/Gospel/Sunday Choir", "Uplifting gospel, choir harmonies, Hammond organ, clapping, joyful", 104.0, 60.0f, 0, true, 4, 1 },
    { "Factory/Folk/Campfire Tales", "Folk, acoustic guitar strumming, banjo, warm harmonies, nostalgic storytelling", 108.0, 60.0f, 0, true, 4, 1 },
    { "Factory/Chiptune/Pixel Quest", "Chiptune, 8-bit square leads, arpeggiated chords, noise drums, playful adventure", 160.0, 30.0f, 0, true, 8, 1 },
    { "Factory/Layer/Add Strings", "Lush string section layer, sustained legato chords, warm cinematic support", 100.0, 30.0f, 0, true, 4, 3 },
    { "Factory/Layer/Add Drums", "Tight live drum kit layer, punchy kick and snare, crisp hats, steady groove", 120.0, 30.0f, 0, true, 4, 3 },
    { "Factory/Repaint/Smooth Transition", "Seamless transition section, gentle riser, smooth fill into the next part", 120.0, 30.0f, 0, true, 4, 2 },
    { "Factory/Reggae/Roots Dub", "Roots reggae, offbeat skank guitar, deep dub bass, spring reverb, relaxed", 76.0, 60.0f, 0, true, 4, 1 },
    { "Factory/Reggae/Dancehall Heat", "Modern dancehall riddim, syncopated drums, bouncy bass, bright synth stabs", 100.0, 45.0f, 0, true, 4, 1 },
    { "Factory/K-Pop/Neon Idol", "Polished K-pop, punchy drums, glossy synths, catchy hook, high energy", 126.0, 60.0f, 0, false, 4, 1 },
    { "Factory/R&B/Velvet Slow Jam", "Smooth contemporary R&B, silky keys, soft 808, intimate vocals, sensual", 72.0, 60.0f, 0, false, 4, 1 },
    { "Factory/R&B/Neo Soul Groove", "Neo soul, Rhodes chords, swung drums, warm bass, lazy pocket", 84.0, 60.0f, 0, true, 4, 1 },
    { "Factory/Punk/Basement Riot", "Fast punk rock, distorted power chords, pounding drums, raw and rebellious", 190.0, 30.0f, 0, true, 6, 1 },
    { "Factory/Shoegaze/Fuzz Haze", "Shoegaze, walls of fuzzy guitars, washed out vocals, dreamy reverb", 112.0, 90.0f, 0, true, 4, 1 },
    { "Factory/Trance/Starlight Ascent", "Uplifting trance, rolling bass, soaring supersaw lead, long breakdown, emotional", 138.0, 90.0f, 0, true, 4, 1 },
    { "Factory/Dubstep/Wobble Quake", "Heavy dubstep, half time drums, growling wobble bass, aggressive drop", 140.0, 45.0f, 0, true, 6, 1 },
    { "Factory/House/Piano Anthem", "Classic piano house, bouncy piano chords, diva vocal chops, uplifting groove", 124.0, 60.0f, 0, true, 4, 1 },
    { "Factory/Garage/Two Step Shuffle", "UK garage two step, shuffled drums, chopped vocals, warm sub bass", 132.0, 45.0f, 0, true, 4, 1 },
    { "Factory/Bluegrass/Mountain Breakdown", "Bluegrass breakdown, rapid banjo rolls, fiddle, upright bass, mandolin chop", 150.0, 45.0f, 0, true, 4, 1 },
    { "Factory/Celtic/Misty Highlands", "Celtic folk, tin whistle melody, bodhran, fiddle, wistful and windswept", 116.0, 60.0f, 0, true, 4, 1 },
    { "Factory/Flamenco/Andalusian Fire", "Flamenco, passionate nylon guitar rasgueado, hand claps, cajon, dramatic", 118.0, 60.0f, 0, true, 4, 1 },
    { "Factory/Bollywood/Monsoon Dance", "Bollywood dance, tabla and dhol, sitar riffs, lush strings, celebratory", 110.0, 60.0f, 0, false, 4, 1 },
    { "Factory/J-Pop/Anime Opening", "Energetic anime opening J-pop, fast drums, bright guitars, triumphant chorus", 170.0, 60.0f, 0, false, 4, 1 },
    { "Factory/Trip-Hop/Smoke Signals", "Trip-hop, slow broken beats, moody bass, vinyl crackle, noir atmosphere", 82.0, 60.0f, 0, true, 4, 1 },
    { "Factory/Vaporwave/Mall Memories", "Vaporwave, slowed smooth jazz samples, lush reverb, retro corporate nostalgia", 66.0, 90.0f, 0, true, 3, 1 },
    { "Factory/Industrial/Factory Floor", "Industrial, clanging metallic percussion, distorted synth bass, cold and mechanical", 118.0, 45.0f, 0, true, 4, 1 },
    { "Factory/Meditation/Singing Bowls", "Meditation music, singing bowls, soft drones, slow breathing pace, serene", 50.0, 180.0f, 0, true, 2, 1 },
    { "Factory/Soundtrack/Retro Arcade Boss", "Video game boss battle, driving rock drums, synth brass, urgent and heroic", 168.0, 45.0f, 0, true, 6, 1 },
    { "Factory/Lullaby/Starry Crib", "Soft lullaby, music box melody, gentle celesta, warm hush, sleepy", 60.0, 60.0f, 0, true, 3, 1 },
    { "Factory/Layer/Add Bassline", "Solid groove bass layer, round tone, locked to the kick, supportive", 110.0, 30.0f, 0, true, 4, 3 },
    { "Factory/Layer/Add Choir Pad", "Airy choir pad layer, sustained ooh and aah voices, wide and soft", 90.0, 30.0f, 0, true, 4, 3 },
    { "Factory/Repaint/Big Ending", "Repaint into a big final ending, crash cymbals, held chord, decisive close", 120.0, 20.0f, 0, true, 4, 2 },
    { "Factory/Funk/Clavinet Strut", "Tight funk, clavinet riff, chicken scratch guitar, punchy horns, syncopated drums", 104.0, 45.0f, 20, true },
    { "Factory/Dubstep/Melodic Skyline", "Melodic dubstep, emotional supersaw chords, heavy sub drop, soaring leads", 150.0, 60.0f, 16, true },
    { "Factory/House/Jackin Basement", "Chicago jackin house, raw drum machine, swung hats, chopped vocal stabs", 125.0, 60.0f, 18, true },
    { "Factory/Garage/Speed Garage Rush", "Speed garage, pitched bass, skippy shuffled drums, rave organ stabs", 132.0, 45.0f, 14, true },
    { "Factory/Gospel/Quiet Hymn", "Gentle gospel hymn, solo piano, soft organ swells, warm choir hum, reverent", 70.0, 60.0f, 4, true },
    { "Factory/Trance/Psy Forest", "Psytrance, rolling offbeat bass, acid squelches, tribal percussion, hypnotic", 142.0, 60.0f, 17, true },
    { "Factory/Punk/Pop Punk Summer", "Pop punk, bright distorted guitars, fast snare, shout along chorus, carefree", 175.0, 45.0f, 3, false, 4, 1 },
    { "Factory/Shoegaze/Pink Static", "Dream pop shoegaze, chorus drenched guitars, slow drums, hazy whispered vocals", 92.0, 60.0f, 9, false, 4, 1 },
    { "Factory/Industrial/Rust Engine", "Industrial rock, grinding guitars, pounding machine drums, distorted shouts", 118.0, 45.0f, 15, true },
    { "Factory/Vaporwave/Hotel Lobby", "Vaporwave, slowed elevator muzak, chorused electric piano, dreamy and surreal", 70.0, 60.0f, 6, true },
    { "Factory/Trip-Hop/Velvet Rain", "Trip-hop, dusty breakbeat, deep upright bass, haunting strings, smoky", 84.0, 60.0f, 20, true },
    { "Factory/Meditation/Ocean Breath", "Meditation, slow ocean waves, warm drone, soft chimes, breathing pace", 50.0, 120.0f, 3, true },
    { "Factory/Lullaby/Moon Cradle", "Lullaby, soft acoustic guitar, humming voice, gentle harp, tender and warm", 60.0, 60.0f, 6, false, 4, 1 },
    { "Factory/Chiptune/Neon Dungeon", "Chiptune dungeon theme, minor key square leads, pulsing triangle bass, tense", 150.0, 45.0f, 22, true },
    { "Factory/Celtic/Reel Gathering", "Fast Celtic reel, fiddle and uilleann pipes, bouzouki, stomping bodhran, lively", 120.0, 45.0f, 3, true },
    { "Factory/Flamenco/Rumba Nights", "Flamenco rumba, strummed nylon guitars, palmas, cajon, warm and festive", 108.0, 45.0f, 22, true },
    { "Factory/Bollywood/Silk Romance", "Romantic Bollywood ballad, bansuri flute, soft tabla, lush strings, tender", 80.0, 60.0f, 15, false, 4, 1 },
    { "Factory/J-Pop/City Pop Drive", "Japanese city pop, glossy electric piano, slap bass, bright brass, breezy", 112.0, 60.0f, 11, false, 4, 1 },
    { "Factory/K-Pop/Dark Concept", "Dark K-pop, hard hitting trap drums, moody synth bass, intense chant hook", 100.0, 60.0f, 24, false, 4, 1 },
    { "Factory/Bluegrass/Sunday Picking", "Easygoing bluegrass, mandolin melody, dobro slides, upright bass, sunny", 112.0, 45.0f, 8, true },
    { "Factory/Folk/Winter Ballad", "Sparse folk ballad, fingerpicked guitar, cello, quiet voice, cold and lonely", 76.0, 60.0f, 17, false, 4, 1 },
    { "Factory/Orchestral/Storm Overture", "Dramatic orchestral overture, thundering timpani, racing strings, bold brass", 132.0, 60.0f, 13, true },
    { "Factory/Soundtrack/Puzzle Room", "Quirky game soundtrack, pizzicato strings, marimba, curious woodwinds", 96.0, 45.0f, 5, true },
    { "Factory/Layer/Add Lead Synth", "Bright lead synth layer, catchy melodic line, glide, sits above the mix", 120.0, 30.0f, 0, true, 4, 3 },
    { "Factory/Repaint/Breakdown Drop", "Repaint into a stripped breakdown, filtered drums, sparse pads, then rebuild", 124.0, 20.0f, 0, true, 4, 2 },
    { "Factory/Techno/Concrete Pulse", "Hypnotic warehouse techno, rumbling kick, metallic percussion, dark stabs", 132.0, 90.0f, 10, true, 3, 1 },
    { "Factory/Drum and Bass/Liquid Skyline", "Liquid drum and bass, rolling breaks, warm sub bass, airy pads, soulful", 174.0, 60.0f, 7, true, 2, 1 },
    { "Factory/Dubstep/Iron Wobble", "Heavy dubstep, growling wobble bass, half time drums, metallic risers", 140.0, 45.0f, 19, true, 6, 1 },
    { "Factory/Synthwave/Chrome Horizon", "Retro synthwave, gated drums, analog brass pads, soaring lead, nostalgic", 108.0, 75.0f, 21, true, 3, 1 },
    { "Factory/Funk/Brass Strut", "Tight funk groove, slap bass, choppy guitar, punchy horn section, swagger", 104.0, 45.0f, 12, true, 5, 1 },
    { "Factory/Disco/Mirror Ball", "Classic disco, four on the floor, octave bass, lush strings, sparkling", 118.0, 90.0f, 2, false, 2, 1 },
    { "Factory/R&B/Velvet Hours", "Smooth R&B slow jam, warm keys, finger snaps, deep bass, sultry vocals", 68.0, 60.0f, 16, false, 3, 1 },
    { "Factory/Soul/Southern Sunday", "Southern soul, Hammond organ, tremolo guitar, gritty heartfelt vocals", 82.0, 75.0f, 1, false, 5, 1 },
    { "Factory/Reggae/Island Porch", "Roots reggae, one drop drums, skank guitar, melodica, deep bass, sunny", 76.0, 60.0f, 8, true, 6, 1 },
    { "Factory/Afrobeat/Lagos Heat", "Afrobeat groove, interlocking guitars, talking drum, horn riffs, energetic", 110.0, 90.0f, 23, true, 2, 1 },
    { "Factory/Latin/Salsa Fuego", "Salsa dura, montuno piano, congas and timbales, blazing trumpets", 180.0, 45.0f, 5, true, 7, 1 },
    { "Factory/Bossa Nova/Rio Window", "Gentle bossa nova, nylon guitar, soft brushes, flute melody, breezy", 132.0, 60.0f, 20, true, 1, 1 },
    { "Factory/Blues/Delta Dust", "Raw delta blues, slide resonator guitar, foot stomp, harmonica, gritty", 72.0, 45.0f, 13, false, 8, 1 },
    { "Factory/Country/Highway Home", "Modern country, twangy telecaster, pedal steel, steady drums, warm vocals", 96.0, 60.0f, 4, false, 4, 1 },
    { "Factory/Metal/Black Iron Gate", "Thrash metal, palm muted riffs, double kick blasts, shredding solo", 196.0, 45.0f, 14, true, 6, 1 },
    { "Factory/Post-Rock/Lantern Field", "Post-rock, tremolo picked guitars, slow crescendo, crashing cymbals", 86.0, 150.0f, 9, true, 2, 1 },
    { "Factory/Classical/Morning Etude", "Solo piano etude, flowing arpeggios, romantic era, expressive dynamics", 92.0, 120.0f, 11, true, 3, 1 },
    { "Factory/Choral/Cathedral Light", "Sacred choral piece, layered voices, long reverb, serene and luminous", 58.0, 90.0f, 18, false, 2, 1 },
    { "Factory/Ambient/Tidal Glass", "Glassy ambient, slow granular piano, soft noise swells, weightless", 64.0, 180.0f, 22, true, 1, 1 },
    { "Factory/Lo-Fi/Attic Sketchbook", "Lo-fi bedroom beat, detuned piano, crunchy drums, cassette hiss, cozy", 86.0, 75.0f, 17, true, 8, 1 },
    { "Factory/Hip-Hop/Rooftop Cypher", "West coast hip-hop, bouncy G-funk synth, talkbox lead, laid back drums", 94.0, 60.0f, 6, true, 5, 1 },
    { "Factory/Cinematic/Desert Pursuit", "Cinematic chase, driving ostinato strings, ethnic percussion, duduk", 150.0, 90.0f, 24, true, 4, 1 },
    { "Factory/Layer/Add String Pad", "Warm string pad layer, slow swells, sustained chords, under the mix", 100.0, 30.0f, 0, true, 3, 3 },
    { "Factory/Layer/Add Percussion", "Shaker and conga percussion layer, light groove, sits around the beat", 116.0, 30.0f, 0, true, 6, 3 },
    { "Factory/Repaint/Vocal Bridge", "Repaint into an emotional vocal bridge, sparse piano, soft strings", 96.0, 15.0f, 0, false, 2, 2 },
    { "Factory/Afrobeat/Highlife Market", "Highlife inspired afrobeat, bright palm wine guitars, shekere, warm horns, easy sway", 116.0, 60.0f, 9, true, 4, 1 },
    { "Factory/Bossa Nova/Quiet Terrace", "Late night bossa nova, soft nylon guitar, brushed snare, muted trumpet, intimate", 124.0, 90.0f, 3, false, 3, 1 },
    { "Factory/Choral/Evensong", "Anglican evensong choir, soaring treble line, organ underneath, stone hall reverb", 64.0, 120.0f, 7, false, 2, 1 },
    { "Factory/Disco/Roller Rink", "Roller disco, funky guitar licks, handclaps, string stabs, falsetto hooks", 122.0, 60.0f, 19, false, 5, 1 },
    { "Factory/Post-Rock/Signal Tower", "Instrumental post-rock, delay soaked guitars, building drums, cathartic peak", 120.0, 180.0f, 14, true, 2, 1 },
    { "Factory/Soul/Motor City Strut", "Classic sixties soul, tambourine backbeat, bright horns, melodic bass, upbeat", 126.0, 60.0f, 10, false, 6, 1 },
    { "Factory/Grunge/Flannel Static", "Grunge, sludgy detuned guitars, quiet verse loud chorus, raw drums, brooding", 98.0, 60.0f, 18, false, 4, 1 },
    { "Factory/Ska/Two Tone Skank", "Two tone ska, upstroke guitar, walking bass, punchy horn section, bouncy", 160.0, 45.0f, 11, true, 5, 1 },
    { "Factory/Swing/Ballroom Jump", "Big band swing, brass shout chorus, walking bass, swinging ride cymbal", 176.0, 60.0f, 4, true, 3, 1 },
    { "Factory/Tango/Buenos Aires Night", "Argentine tango, bandoneon, staccato strings, dramatic piano, sharp accents", 116.0, 75.0f, 23, true, 4, 1 },
    { "Factory/Reggaeton/Perreo Heat", "Reggaeton, dembow drum pattern, deep 808, catchy synth plucks, party energy", 94.0, 45.0f, 13, false, 6, 1 },
    { "Factory/Phonk/Drift Smoke", "Drift phonk, distorted cowbell melody, heavy 808, memphis vocal chops, aggressive", 150.0, 30.0f, 12, true, 8, 1 },
    { "Factory/Hyperpop/Sugar Glitch", "Hyperpop, pitched up vocals, crunchy distorted synths, glitchy drums, maximal", 160.0, 45.0f, 2, false, 6, 1 },
    { "Factory/Grime/Estate Pressure", "Grime, square wave bass, cold eskibeat synths, sparse hard drums, tense", 140.0, 45.0f, 21, true, 4, 1 },
    { "Factory/Breakbeat/Big Beat Riot", "Big beat breakbeat, chopped funk breaks, acid line, distorted riffs, rowdy", 132.0, 60.0f, 5, true, 4, 1 },
    { "Factory/New Age/Crystal Valley", "New age, warm synth pads, pan flute, soft harp arpeggios, gentle and healing", 72.0, 150.0f, 8, true, 2, 1 },
    { "Factory/Downtempo/Sunset Balcony", "Downtempo chillout, mellow beats, dub bass, airy guitar, balearic glow", 98.0, 90.0f, 16, true, 3, 1 },
    { "Factory/Polka/Village Square", "Lively polka, accordion melody, oom pah tuba, clarinet, bright and festive", 120.0, 45.0f, 1, true, 4, 1 },
    { "Factory/Sea Shanty/Harbor Chorus", "Sea shanty, rousing male group vocals, stomps, fiddle and concertina", 104.0, 60.0f, 15, false, 5, 1 },
    { "Factory/Marching Band/Homecoming", "Marching band, snare cadence, bold brass melody, cymbal crashes, proud", 112.0, 60.0f, 20, true, 3, 1 },
    { "Factory/Baroque/Harpsichord Fugue", "Baroque fugue, harpsichord counterpoint, continuo cello, ornate and precise", 100.0, 120.0f, 24, true, 2, 1 },
    { "Factory/Noise/Feedback Wall", "Harsh noise rock, screaming feedback, pummeling drums, chaotic and abrasive", 170.0, 30.0f, 6, true, 7, 1 },
    { "Factory/Lounge/Cocktail Hour", "Exotica lounge, vibraphone, bongos, plush strings, easy listening charm", 108.0, 75.0f, 12, true, 1, 1 },
    { "Factory/Repaint/Intro Swell", "Repaint into a soft intro swell, filtered pads, gentle rise into the groove", 110.0, 15.0f, 0, true, 3, 2 },
    { "Factory/Repaint/Guitar Solo Break", "Repaint into an expressive guitar solo break, bending leads over the band", 128.0, 20.0f, 0, true, 5, 2 },
#if defined (HSLINK_TEETH_PRESETS)
    { "Factory/EDM/Night Drive",        "Duplicate entry (teeth)", 124.0, 60.0f, 0, true },
#endif
};

inline constexpr int kCount = (int) (sizeof (kEntries) / sizeof (kEntries[0]));

inline bool isFactoryName (const juce::String& name) { return name.startsWith (kPrefix); }

inline int indexOf (const juce::String& name)
{
    for (int i = 0; i < kCount; ++i)
        if (name == kEntries[i].name)
            return i;
    return -1;
}

inline HotStepLinkProcessor::GenSettings toSettings (const Entry& e)
{
    HotStepLinkProcessor::GenSettings s;
    s.caption = e.caption;
    s.lyrics = {};
    s.instrumental = e.instrumental;
    s.bpm = e.bpm;
    s.duration = e.duration;
    s.takes = e.takes;
    s.seed = -1;
    s.keyIndex = e.keyIndex;
    s.modeId = e.modeId;
    s.backend = 0;
    s.aceModel = 0;
    return s;
}
} // namespace FactoryPresets
