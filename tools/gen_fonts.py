#!/usr/bin/env python3
"""日本語サブセットフォント生成 (tools/gen_fonts.py)

firmware/components/ui/src/fonts/ に LVGL フォントを生成する。
  - font_jp_20/26      : UI 文言用 (ASCII + かな + CJK句読点 + 全角記号
                          + UI ソース内で使われている漢字 + 予備の常用漢字)
  - font_digits_56/96  : 時刻・タイマー用 (0-9 : . - + のみ)
  - font_fc_<fam>_<px> : 文字盤の時計数字用 (0-9 : のみ)。
                          <fam> = oswald / bebas / orbitron / outfit / chakra

使い方:
  python3 tools/gen_fonts.py --font /path/to/NotoSansJP.ttf
  python3 tools/gen_fonts.py --faces          # 文字盤フォントだけ生成

依存: lv_font_conv (npm i -g lv_font_conv)
フォント: Noto Sans JP + Google Fonts 5 種 (OFL、tools/fonts/face/)。
          生成物と一緒に OFL.txt / OFL-face.txt を同梱すること。
"""

import argparse
import pathlib
import re
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
UI_SRC = REPO / "firmware" / "components" / "ui" / "src"
FONT_OUT = UI_SRC / "fonts"

# かな・句読点・全角ASCII は範囲指定 (よく使う文字を漏らさないため)。
BASE_RANGES = ("0x20-0x7E,0x3000-0x303F,0x3040-0x30FF,0xFF01-0xFF5E,"
              "0x2039-0x203A")  # ‹ › (ヘッダの戻る/行のシェブロン)

# UI ソース以外で表示したい漢字の予備 (曜日・単位・定型文用)。
# 常用漢字 (2010年告示 2136字)。メモ等のユーザー入力で豆腐にならないよう同梱。
JOYO_KANJI = (
    "亜哀挨愛曖悪握圧扱宛嵐安案暗以衣位囲医依委威為畏胃尉異移萎偉椅彙意違維慰遺緯域育一壱逸茨芋引印因咽姻員院淫陰飲隠韻右宇羽雨唄鬱畝浦運雲永泳英映栄営詠影鋭衛易疫益"
    "液駅悦越謁閲円延沿炎怨宴媛援園煙猿遠鉛塩演縁艶汚王凹央応往押旺欧殴桜翁奥横岡屋億憶臆虞乙俺卸音恩温穏下化火加可仮何花佳価果河苛科架夏家荷華菓貨渦過嫁暇禍靴寡歌箇"
    "稼課蚊牙瓦我画芽賀雅餓介回灰会快戒改怪拐悔海界皆械絵開階塊楷解潰壊懐諧貝外劾害崖涯街慨蓋該概骸垣柿各角拡革格核殻郭覚較隔閣確獲嚇穫学岳楽額顎掛潟括活喝渇割葛滑褐"
    "轄且株釜鎌刈干刊甘汗缶完肝官冠巻看陥乾勘患貫寒喚堪換敢棺款間閑勧寛幹感漢慣管関歓監緩憾還館環簡観韓艦鑑丸含岸岩玩眼頑顔願企伎危机気岐希忌汽奇祈季紀軌既記起飢鬼帰"
    "基寄規亀喜幾揮期棋貴棄毀旗器畿輝機騎技宜偽欺義疑儀戯擬犠議菊吉喫詰却客脚逆虐九久及弓丘旧休吸朽臼求究泣急級糾宮救球給嗅窮牛去巨居拒拠挙虚許距魚御漁凶共叫狂京享供"
    "協況峡挟狭恐恭胸脅強教郷境橋矯鏡競響驚仰暁業凝曲局極玉巾斤均近金菌勤琴筋僅禁緊錦謹襟吟銀区句苦駆具惧愚空偶遇隅串屈掘窟熊繰君訓勲薫軍郡群兄刑形系径茎係型契計恵啓"
    "掲渓経蛍敬景軽傾携継詣慶憬稽憩警鶏芸迎鯨隙劇撃激桁欠穴血決結傑潔月犬件見券肩建研県倹兼剣拳軒健険圏堅検嫌献絹遣権憲賢謙鍵繭顕験懸元幻玄言弦限原現舷減源厳己戸古呼"
    "固股虎孤弧故枯個庫湖雇誇鼓錮顧五互午呉後娯悟碁語誤護口工公勾孔功巧広甲交光向后好江考行坑孝抗攻更効幸拘肯侯厚恒洪皇紅荒郊香候校耕航貢降高康控梗黄喉慌港硬絞項溝鉱"
    "構綱酵稿興衡鋼講購乞号合拷剛傲豪克告谷刻国黒穀酷獄骨駒込頃今困昆恨根婚混痕紺魂墾懇左佐沙査砂唆差詐鎖座挫才再災妻采砕宰栽彩採済祭斎細菜最裁債催塞歳載際埼在材剤財"
    "罪崎作削昨柵索策酢搾錯咲冊札刷刹拶殺察撮擦雑皿三山参桟蚕惨産傘散算酸賛残斬暫士子支止氏仕史司四市矢旨死糸至伺志私使刺始姉枝祉肢姿思指施師恣紙脂視紫詞歯嗣試詩資飼"
    "誌雌摯賜諮示字寺次耳自似児事侍治持時滋慈辞磁餌璽鹿式識軸七𠮟失室疾執湿嫉漆質実芝写社車舎者射捨赦斜煮遮謝邪蛇尺借酌釈爵若弱寂手主守朱取狩首殊珠酒腫種趣寿受呪授需"
    "儒樹収囚州舟秀周宗拾秋臭修袖終羞習週就衆集愁酬醜蹴襲十汁充住柔重従渋銃獣縦叔祝宿淑粛縮塾熟出述術俊春瞬旬巡盾准殉純循順準潤遵処初所書庶暑署緒諸女如助序叙徐除小升"
    "少召匠床抄肖尚招承昇松沼昭宵将消症祥称笑唱商渉章紹訟勝掌晶焼焦硝粧詔証象傷奨照詳彰障憧衝賞償礁鐘上丈冗条状乗城浄剰常情場畳蒸縄壌嬢錠譲醸色拭食植殖飾触嘱織職辱尻"
    "心申伸臣芯身辛侵信津神唇娠振浸真針深紳進森診寝慎新審震薪親人刃仁尽迅甚陣尋腎須図水吹垂炊帥粋衰推酔遂睡穂随髄枢崇数据杉裾寸瀬是井世正生成西声制姓征性青斉政星牲省"
    "凄逝清盛婿晴勢聖誠精製誓静請整醒税夕斥石赤昔析席脊隻惜戚責跡積績籍切折拙窃接設雪摂節説舌絶千川仙占先宣専泉浅洗染扇栓旋船戦煎羨腺詮践箋銭潜線遷選薦繊鮮全前善然禅"
    "漸膳繕狙阻祖租素措粗組疎訴塑遡礎双壮早争走奏相荘草送倉捜挿桑巣掃曹曽爽窓創喪痩葬装僧想層総遭槽踪操燥霜騒藻造像増憎蔵贈臓即束足促則息捉速側測俗族属賊続卒率存村孫"
    "尊損遜他多汰打妥唾堕惰駄太対体耐待怠胎退帯泰堆袋逮替貸隊滞態戴大代台第題滝宅択沢卓拓託濯諾濁但達脱奪棚誰丹旦担単炭胆探淡短嘆端綻誕鍛団男段断弾暖談壇地池知値恥致"
    "遅痴稚置緻竹畜逐蓄築秩窒茶着嫡中仲虫沖宙忠抽注昼柱衷酎鋳駐著貯丁弔庁兆町長挑帳張彫眺釣頂鳥朝貼超腸跳徴嘲潮澄調聴懲直勅捗沈珍朕陳賃鎮追椎墜通痛塚漬坪爪鶴低呈廷弟"
    "定底抵邸亭貞帝訂庭逓停偵堤提程艇締諦泥的笛摘滴適敵溺迭哲鉄徹撤天典店点展添転塡田伝殿電斗吐妬徒途都渡塗賭土奴努度怒刀冬灯当投豆東到逃倒凍唐島桃討透党悼盗陶塔搭棟"
    "湯痘登答等筒統稲踏糖頭謄藤闘騰同洞胴動堂童道働銅導瞳峠匿特得督徳篤毒独読栃凸突届屯豚頓貪鈍曇丼那奈内梨謎鍋南軟難二尼弐匂肉虹日入乳尿任妊忍認寧熱年念捻粘燃悩納能"
    "脳農濃把波派破覇馬婆罵拝杯背肺俳配排敗廃輩売倍梅培陪媒買賠白伯拍泊迫剝舶博薄麦漠縛爆箱箸畑肌八鉢発髪伐抜罰閥反半氾犯帆汎伴判坂阪板版班畔般販斑飯搬煩頒範繁藩晩番"
    "蛮盤比皮妃否批彼披肥非卑飛疲秘被悲扉費碑罷避尾眉美備微鼻膝肘匹必泌筆姫百氷表俵票評漂標苗秒病描猫品浜貧賓頻敏瓶不夫父付布扶府怖阜附訃負赴浮婦符富普腐敷膚賦譜侮武"
    "部舞封風伏服副幅復福腹複覆払沸仏物粉紛雰噴墳憤奮分文聞丙平兵併並柄陛閉塀幣弊蔽餅米壁璧癖別蔑片辺返変偏遍編弁便勉歩保哺捕補舗母募墓慕暮簿方包芳邦奉宝抱放法泡胞俸"
    "倣峰砲崩訪報蜂豊飽褒縫亡乏忙坊妨忘防房肪某冒剖紡望傍帽棒貿貌暴膨謀頰北木朴牧睦僕墨撲没勃堀本奔翻凡盆麻摩磨魔毎妹枚昧埋幕膜枕又末抹万満慢漫未味魅岬密蜜脈妙民眠矛"
    "務無夢霧娘名命明迷冥盟銘鳴滅免面綿麺茂模毛妄盲耗猛網目黙門紋問冶夜野弥厄役約訳薬躍闇由油喩愉諭輸癒唯友有勇幽悠郵湧猶裕遊雄誘憂融優与予余誉預幼用羊妖洋要容庸揚揺"
    "葉陽溶腰様瘍踊窯養擁謡曜抑沃浴欲翌翼拉裸羅来雷頼絡落酪辣乱卵覧濫藍欄吏利里理痢裏履璃離陸立律慄略柳流留竜粒隆硫侶旅虜慮了両良料涼猟陵量僚領寮療瞭糧力緑林厘倫輪隣"
    "臨瑠涙累塁類令礼冷励戻例鈴零霊隷齢麗暦歴列劣烈裂恋連廉練錬呂炉賂路露老労弄郎朗浪廊楼漏籠六録麓論和話賄脇惑枠湾腕"
)

EXTRA_KANJI = (
    "曜日月火水木金土日年時分秒電池充接続未開始停止設定画面"
    "明消再起動源強制押長端末情報開発者録音決定戻削除一覧追加"
    "注意終止現在無数値確認中利用可能状態更新日時変更保存必要警告"
)

DIGIT_SYMBOLS = "0123456789:.-+"

# 文字盤フォント (Google Fonts / OFL、tools/fonts/face/ に TTF を置く)。
#   (生成シンボル名の family 部, サイズpx, TTF ファイル名)
#   Oswald は bold 文字盤の「時=太/分=細」再現用に 150px で2ウェイト。
FACE_FONT_TTF_DIR = REPO / "tools" / "fonts" / "face"
FACE_FONT_SPECS = [
    ("oswald",   150, "Oswald-600.ttf"),
    ("oswald_l", 150, "Oswald-300.ttf"),
    ("oswald",   112, "Oswald-600.ttf"),
    ("oswald",    34, "Oswald-400.ttf"),
    ("oswald",    18, "Oswald-600.ttf"),
    ("bebas",    150, "BebasNeue-400.ttf"),
    ("bebas",    112, "BebasNeue-400.ttf"),
    ("bebas",     34, "BebasNeue-400.ttf"),
    ("bebas",     18, "BebasNeue-400.ttf"),
    ("orbitron", 150, "Orbitron-700.ttf"),
    ("orbitron", 112, "Orbitron-700.ttf"),
    ("orbitron",  34, "Orbitron-700.ttf"),
    ("orbitron",  18, "Orbitron-700.ttf"),
    ("outfit",   150, "Outfit-200.ttf"),
    ("outfit",   112, "Outfit-200.ttf"),
    ("outfit",    34, "Outfit-300.ttf"),
    ("outfit",    18, "Outfit-300.ttf"),
    ("chakra",   150, "ChakraPetch-500.ttf"),
    ("chakra",   112, "ChakraPetch-500.ttf"),
    ("chakra",    34, "ChakraPetch-500.ttf"),
    ("chakra",    18, "ChakraPetch-500.ttf"),
]
FACE_FONT_SYMBOLS = "0123456789:."

STRING_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')


def collect_ui_chars() -> str:
    """UI ソースの文字列リテラルに含まれる非ASCII文字を全部拾う。"""
    chars = set(EXTRA_KANJI) | set(JOYO_KANJI)
    for path in sorted(UI_SRC.rglob("*.cpp")) + sorted(UI_SRC.rglob("*.hpp")):
        text = path.read_text(encoding="utf-8")
        for m in STRING_RE.finditer(text):
            for ch in m.group(1):
                if ord(ch) > 0x7E and ord(ch) not in (0x2026,):
                    chars.add(ch)
    # LV_LABEL_LONG_DOT 用 (U+2026)
    chars.add("…")
    return "".join(sorted(chars))


def run(args: list[str]) -> None:
    print("$", " ".join(args))
    subprocess.run(args, check=True)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--font", help="NotoSansJP.ttf のパス (省略時は日本語系を生成しない)")
    ap.add_argument("--faces", action="store_true",
                    help="文字盤フォントを tools/fonts/face/ から生成する")
    ap.add_argument("--face-ttf-dir", default=str(FACE_FONT_TTF_DIR),
                    help="文字盤フォント TTF の場所")
    ap.add_argument("--out", default=str(FONT_OUT))
    ap.add_argument("--bpp", default="4")
    args = ap.parse_args()

    if not args.font and not args.faces:
        ap.error("--font か --faces のどちらかを指定して")

    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    if args.font:
        symbols = collect_ui_chars()
        print(f"UI chars: {len(symbols)} codepoints (incl. EXTRA_KANJI)")

        # 日本語フォント (範囲 + 文字列の和)
        for size in (20, 26):
            run([
                "lv_font_conv",
                "--font", args.font,
                "--size", str(size),
                "--bpp", args.bpp,
                "--format", "lvgl",
                "--lv-include", "lvgl.h",
                "-r", BASE_RANGES,
                "--symbols", symbols,
                "--lv-font-name", f"font_jp_{size}",
                "-o", str(out / f"font_jp_{size}.c"),
            ])

        # 数字フォント
        for size in (56, 96):
            run([
                "lv_font_conv",
                "--font", args.font,
                "--size", str(size),
                "--bpp", args.bpp,
                "--format", "lvgl",
                "--lv-include", "lvgl.h",
                "--symbols", DIGIT_SYMBOLS,
                "--lv-font-name", f"font_digits_{size}",
                "-o", str(out / f"font_digits_{size}.c"),
            ])

    if args.faces:
        ttf_dir = pathlib.Path(args.face_ttf_dir)
        for fam, size, ttf in FACE_FONT_SPECS:
            src = ttf_dir / ttf
            if not src.exists():
                sys.exit(f"missing TTF: {src}")
            name = f"font_fc_{fam}_{size}"
            run([
                "lv_font_conv",
                "--font", str(src),
                "--size", str(size),
                "--bpp", args.bpp,
                "--format", "lvgl",
                "--lv-include", "lvgl.h",
                "--symbols", FACE_FONT_SYMBOLS,
                "--lv-font-name", name,
                "-o", str(out / f"{name}.c"),
            ])

    print(f"done -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
