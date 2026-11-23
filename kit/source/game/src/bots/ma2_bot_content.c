/* MechAssault 2 offline bots - content module (localized menu strings and
 * pilot callsigns). Implements only the "Content" section of
 * bots/shared/ma2_bot_api.h.
 *
 * Source is pure ASCII: every non-ASCII character is a \uXXXX escape so the
 * file compiles identically with or without /utf-8.
 */
#include "ma2_bot_api.h"

/* Index order must match enum Ma2BotText: BOTS, RED_BOTS, BLUE_BOTS,
 * BOT_SKILL, OFF, SKILL_RECRUIT, SKILL_REGULAR, SKILL_VETERAN, SKILL_ELITE, SKILL_MIXED,
 * DESC_RECRUIT, DESC_REGULAR, DESC_VETERAN, DESC_ELITE, DESC_MIXED.
 * Row order must match enum Ma2Language: EN, FR, DE, IT, JA, ES. */
static const wchar_t *const ma2_bot_text_table[MA2_LANG_COUNT][MA2_TEXT_COUNT] = {
    /* English */
    {
        L"Bots:", L"Red Bots:", L"Blue Bots:", L"Bot Skill:", L"Off",
        L"Recruit", L"Regular", L"Veteran", L"Elite", L"Mixed",
        L"Fresh from the academy. Slow on the trigger and easily rattled.", L"Line pilots who know their machines but rarely surprise you.", L"Hardened pilots. They flank, take cover and punish mistakes.", L"The deadliest aces in the Inner Sphere. Expect no mercy.", L"A real lobby: green rookies, hardened aces and the odd Dragoon legend side by side.",
        L"Dragoon", L"Legendary mercenary MechWarriors. They read your every move, strike as one and never let up.",
        L"Bots on this team. A team holds 6 players, you included; you join the side the counts leave short.",
        L"How many bots you fight (up to 11). Choose Off to explore the map on your own.",
        L"Grinder is two-player co-op: one bot partner at most."
    },
    /* French */
    {
        L"Bots :", L"Bots Rouges :", L"Bots Bleus :", L"Niveau des Bots :", L"Non",
        L"Recrue", L"Standard", L"Vétéran", L"Élite", L"Mixte",
        L"Tout juste sortis de l'académie. Lents à tirer et vite déstabilisés.", L"Des pilotes de ligne qui connaissent leur machine, sans plus.", L"Des pilotes aguerris. Ils contournent, se couvrent et punissent les erreurs.", L"Les as les plus redoutables de la Sphère intérieure. Aucune pitié.", L"Un vrai salon : recrues, as aguerris et parfois un Dragon légendaire côte à côte.",
        L"Dragon", L"Des MechWarriors mercenaires légendaires. Ils lisent chacun de vos gestes, frappent ensemble et ne lâchent jamais rien.",
        L"Bots de cette équipe. Une équipe compte 6 joueurs, vous compris ; vous rejoignez le camp que les nombres laissent incomplet.",
        L"Bots adversaires, jusqu'à 11. Non : vous jouez seul sur la carte.",
        L"Grinder se joue à deux en coopération : un seul bot partenaire au plus."
    },
    /* German */
    {
        L"Bots:", L"Rote Bots:", L"Blaue Bots:", L"Bot-Stufe:", L"Aus",
        L"Rekrut", L"Regulär", L"Veteran", L"Elite", L"Gemischt",
        L"Frisch von der Akademie. Zögern beim Schuss und schnell aus der Fassung.", L"Linienpiloten, die ihre Maschine kennen, aber selten überraschen.", L"Abgehärtete Piloten. Sie flankieren, suchen Deckung und bestrafen Fehler.", L"Die tödlichsten Asse der Inneren Sphäre. Keine Gnade.", L"Eine echte Lobby: grüne Rekruten, erfahrene Asse und mitunter ein legendärer Dragoner Seite an Seite.",
        L"Dragoner", L"Legendäre Söldner-MechKrieger. Sie durchschauen jeden Zug, schlagen gemeinsam zu und lassen nie locker.",
        L"Bots in diesem Team. Ein Team hat 6 Spieler, Sie eingeschlossen; Sie kommen auf die Seite, die die Zahlen kürzer lassen.",
        L"Bot-Gegner, bis zu 11. Aus: Sie spielen die Karte allein.",
        L"Grinder ist Koop für zwei: höchstens ein Bot als Partner."
    },
    /* Italian */
    {
        L"Bot:", L"Bot Rossi:", L"Bot Blu:", L"Livello Bot:", L"Disattivo",
        L"Recluta", L"Regolare", L"Veterano", L"Élite", L"Misto",
        L"Appena usciti dall'accademia. Lenti a sparare e facili da scuotere.", L"Piloti di linea che conoscono la loro macchina, ma sorprendono di rado.", L"Piloti temprati. Aggirano, si coprono e puniscono gli errori.", L"Gli assi più letali della Sfera Interna. Nessuna pietà.", L"Una vera lobby: reclute, assi temprati e a volte un Dragone leggendario fianco a fianco.",
        L"Dragone", L"MechWarrior mercenari leggendari. Leggono ogni tua mossa, colpiscono all'unisono e non mollano mai.",
        L"Bot di questa squadra. Una squadra conta 6 giocatori, te compreso; entri nella squadra che i numeri lasciano più corta.",
        L"Bot avversari, fino a 11. Disattivo: giochi la mappa da solo.",
        L"Grinder è cooperativo per due: al massimo un bot compagno."
    },
    /* Japanese: ボット / レッドボット / ブルーボット / ボットの強さ / オフ /
     * 新兵 / 通常 / ベテラン / エリート */
    {
        L"ボット：",
        L"レッドボット：",
        L"ブルーボット：",
        L"ボットの強さ：",
        L"オフ",
        L"新兵",
        L"通常",
        L"ベテラン",
        L"エリート",
        L"ミックス",
        L"士官学校を出たばかり。反応が遅く、すぐに動揺する。",
        L"機体を知る正規パイロット。だが意外性はない。",
        L"歴戦のパイロット。側面を突き、遮蔽を使い、ミスを逃さない。",
        L"インナースフィア最強のエース。容赦はない。",
        L"本物のロビー：新兵、歴戦のエース、そして時に伝説のドラグーンが入り混じる。",
        L"ドラグーン", L"伝説の傭兵メックウォリアー。あらゆる動きを読み、連携して攻め、決して手を緩めない。",
        L"このチームのボット。1チームは自分を含めて6人。人数の少ない側に入ります。",
        L"敵ボットは最大11体。オフにするとマップを一人でプレイします。",
        L"グラインダーは2人協力プレイ：味方ボットは最大1体。"
    },
    /* Spanish */
    {
        L"Bots:", L"Bots Rojos:", L"Bots Azules:", L"Nivel de Bots:", L"Desactivado",
        L"Recluta", L"Regular", L"Veterano", L"Élite", L"Mixto",
        L"Recién salidos de la academia. Lentos con el gatillo y fáciles de alterar.", L"Pilotos de línea que conocen su máquina, pero rara vez sorprenden.", L"Pilotos curtidos. Flanquean, se cubren y castigan los errores.", L"Los ases más letales de la Esfera Interior. Sin piedad.", L"Una partida real: novatos, ases curtidos y algún Dragón legendario codo con codo.",
        L"Dragón", L"Legendarios MechWarriors mercenarios. Leen cada movimiento, atacan como uno solo y nunca dan tregua.",
        L"Bots de este equipo. Un equipo tiene 6 jugadores, tú incluido; entras en el bando que los números dejan corto.",
        L"Bots rivales, hasta 11. Desactivado: juegas el mapa solo.",
        L"Grinder es cooperativo para dos: como máximo un bot compañero."
    }
};

static const wchar_t *const ma2_bot_callsigns[] = {
    L"Kerensky",
    L"Vortex",
    L"Sabre",
    L"Wraith",
    L"Ember",
    L"Cobra",
    L"Hellcat",
    L"Ironside",
    L"Raptor",
    L"Nomad",
    L"Ghost",
    L"Viper",
    L"Falcon",
    L"Talon",
    L"Striker",
    L"Blackjack",
    L"Havoc",
    L"Tempest",
    L"Anvil",
    L"Hornet",
    L"Cutlass",
    L"Mercer",
    L"Rook",
    L"Cinder",
    L"Stormcrow",
    L"Marauder",
    L"Gunnar",
    L"Obsidian"
};

#define MA2_BOT_CALLSIGN_COUNT ((int)(sizeof(ma2_bot_callsigns) / sizeof(ma2_bot_callsigns[0])))

const wchar_t *ma2_bot_text(int text, int language)
{
    if (text < 0 || text >= MA2_TEXT_COUNT)
        return NULL;
    if (language < 0 || language >= MA2_LANG_COUNT)
        language = MA2_LANG_ENGLISH;
    return ma2_bot_text_table[language][text];
}

int ma2_bot_callsign_count(void)
{
    return MA2_BOT_CALLSIGN_COUNT;
}

const wchar_t *ma2_bot_callsign(int index)
{
    int n = MA2_BOT_CALLSIGN_COUNT;
    int i = index % n;
    if (i < 0)
        i += n;
    return ma2_bot_callsigns[i];
}
