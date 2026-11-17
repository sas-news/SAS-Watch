#!/usr/bin/env python3
"""日本語サブセットフォント生成 (tools/gen_fonts.py)

firmware/components/ui/src/fonts/ に LVGL フォントを生成する。
T24 以降、日本語フォントの実体は assets パーティションの
fonts/jp_<px>.bin (lv_binfont) で、バイナリにはフォールバック用の
最小フォント font_jp_min_20 (ASCII+かな+記号) だけが残る。
  - font_jp_min_20     : binfont が読めない時の予備 (ASCII+かな+記号)
  - tools/fonts/bin/jp_20/26/32.bin : 本体 (--jp-bin で生成。
                          20/26 は常用漢字込み、32 はソースの漢字のみ)
  - font_digits_56/96  : 時刻・タイマー用 (0-9 : . - + のみ)
  - font_fc_<fam>_<px> : 文字盤の時計数字用 (0-9 : - . / のみ)。
                          <fam> = oswald / bebas / orbitron / outfit / chakra
                          T43: 以前は ASCII 0x20-0x7E (95 グリフ) を焼いていたが、
                          faces/ の digits()/digits_thin() は数字と記号しか描かない
                          ため 0x2D-0x3A (14 グリフ) に絞った (flash 約 653KB 減)。
  - font_num_40/72     : 機能画面の等幅数字用 (0-9 : , . - / % 空白)。
                          JetBrains Mono Bold (OFL、tools/fonts/num/)

使い方:
  python3 tools/gen_fonts.py --jp-bin         # 本体 .bin + font_jp_min_20.c
  python3 tools/gen_fonts.py --font /path/to/NotoSansJP.ttf
  python3 tools/gen_fonts.py --faces          # 文字盤フォントだけ生成
  python3 tools/gen_fonts.py --num            # 等幅数字フォントだけ生成

依存: lv_font_conv 1.5.3+ (npm i -g lv_font_conv。PATH に無ければ
      .toolchain/npm-cache か ~/.npm/_npx のキャッシュを探す)
フォント: M PLUS 1p (OFL、tools/fonts/jp/) + Google Fonts 5 種
          (OFL、tools/fonts/face/) + JetBrains Mono (OFL、tools/fonts/num/)。
          生成物と一緒に OFL.txt / OFL-face.txt を同梱すること。
"""

import argparse
import os
import pathlib
import re
import shutil
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
UI_SRC = REPO / "firmware" / "components" / "ui" / "src"
# 文字列リテラルの収集はアプリ側も対象 ("−" 等が apps/ にだけ出る)。
UI_SRC_SCAN = [UI_SRC, REPO / "apps"]
FONT_OUT = UI_SRC / "fonts"

# かな・句読点・全角ASCII は範囲指定 (よく使う文字を漏らさないため)。
BASE_RANGES = ("0x20-0x7E,0x2010-0x2027,0x3000-0x303F,0x3040-0x30FF,"
              "0xFF01-0xFF5E")  # ASCII + ダッシュ/引用/… + 和文記号/かな/全角

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
    ("oswald",    96, "Oswald-600.ttf"),
    ("oswald",    34, "Oswald-400.ttf"),
    ("oswald",    18, "Oswald-600.ttf"),
    ("bebas",    150, "BebasNeue-400.ttf"),
    ("bebas",    112, "BebasNeue-400.ttf"),
    ("bebas",     96, "BebasNeue-400.ttf"),
    ("bebas",     34, "BebasNeue-400.ttf"),
    ("bebas",     18, "BebasNeue-400.ttf"),
    ("orbitron", 150, "Orbitron-700.ttf"),
    ("orbitron", 112, "Orbitron-700.ttf"),
    ("orbitron",  96, "Orbitron-700.ttf"),
    ("orbitron",  34, "Orbitron-700.ttf"),
    ("orbitron",  18, "Orbitron-700.ttf"),
    ("outfit",   150, "Outfit-200.ttf"),
    ("outfit",   112, "Outfit-200.ttf"),
    ("outfit",    96, "Outfit-200.ttf"),
    ("outfit",    34, "Outfit-300.ttf"),
    ("outfit",    18, "Outfit-300.ttf"),
    ("chakra",   150, "ChakraPetch-500.ttf"),
    ("chakra",   112, "ChakraPetch-500.ttf"),
    ("chakra",    96, "ChakraPetch-500.ttf"),
    ("chakra",    34, "ChakraPetch-500.ttf"),
    ("chakra",    18, "ChakraPetch-500.ttf"),
]
# 文字盤フォントのグリフ範囲 (T43)。
#   faces/ の digits()/digits_thin() が描く文字は数字・コロン・マイナス・
#   ドット・スラッシュだけ (全 call site を確認: bold/hud/minimal/chara_side/
#   chara_bubble の時刻と analog の 12/3/6/9・日付)。ASCII 全部 (95 グリフ) を
#   焼くと 26 フォントで 862KB が app に載るため、0x2D-0x3A (14 グリフ) に絞る。
#   空白は数字ラベルに現れないので含めない。英字を digits() で描くと豆腐に
#   なるので、その時はここを広げて再生成する (docs/lightweight.md §2.3)。
FACE_FONT_RANGE = "0x2D-0x3A"  # - . / 0-9 :

# サブセット生成では line_height/base_line が「含めたグリフの最大高さ」から
# 再計算され、ASCII 全量で生成した現行フォントと値が変わる (例: oswald 150 は
# 165/29 → 126/3)。LVGL はこの 2 値でラベル内のベースラインを決めるため、
# 変えると数字盤の文字位置が最大十数 px ずれる。見た目を変えないため、
# ASCII 全量時代の値をここに固定して生成後に書き戻す (T43)。
FACE_FONT_METRICS = {
    "font_fc_bebas_112": (120, 23),
    "font_fc_bebas_150": (161, 30),
    "font_fc_bebas_18": (21, 4),
    "font_fc_bebas_34": (36, 7),
    "font_fc_bebas_96": (103, 19),
    "font_fc_chakra_112": (120, 25),
    "font_fc_chakra_150": (160, 33),
    "font_fc_chakra_18": (19, 4),
    "font_fc_chakra_34": (36, 7),
    "font_fc_chakra_96": (102, 21),
    "font_fc_orbitron_112": (119, 26),
    "font_fc_orbitron_150": (158, 34),
    "font_fc_orbitron_18": (20, 4),
    "font_fc_orbitron_34": (37, 8),
    "font_fc_orbitron_96": (102, 22),
    "font_fc_oswald_112": (124, 22),
    "font_fc_oswald_150": (165, 29),
    "font_fc_oswald_18": (20, 3),
    "font_fc_oswald_34": (38, 7),
    "font_fc_oswald_96": (107, 19),
    "font_fc_oswald_l_150": (164, 28),
    "font_fc_outfit_112": (111, 23),
    "font_fc_outfit_150": (149, 31),
    "font_fc_outfit_18": (19, 4),
    "font_fc_outfit_34": (34, 7),
    "font_fc_outfit_96": (96, 20),
}

# ASCII 全量 (95 グリフ) 時代のカーニングを、残したグリフの組だけ抜き出して
# 固定化した参照表 (T43)。サブセット生成では lv_font_conv がグリフ数に応じて
# kern_scale と形式 (pairs / classes) を選び直すため、そのままだと
# 「41」などの数字ペアの送り幅が 1/16px ずれ、中央寄せラベルが 1px 動く
# (sim 差分で 3 画面に出た)。値と scale を当時に戻して見た目を一致させる。
#   形式: "<kern_scale>|<2文字ペア>=<int8値>,..."
FACE_FONT_KERN = {
    "font_fc_bebas_112": "34|//=-118",
    "font_fc_bebas_150": "45|//=-119",
    "font_fc_bebas_18": "16|//=-40",
    "font_fc_bebas_34": "16|//=-76",
    "font_fc_bebas_96": "29|//=-119",
    "font_fc_chakra_112": "27|-7=-37,.0=-21,.1=-80,.3=-16,.4=-11,.5=-16,.6=-16,.7=-32,.8=-16,.9=-16,0.=-21,3.=-16,4.=-11,5.=-16,6.=-16,7-=-48,7.=-90,74=-48,8.=-16,9.=-16",
    "font_fc_chakra_150": "36|-7=-37,.0=-21,.1=-80,.3=-16,.4=-11,.5=-16,.6=-16,.7=-32,.8=-16,.9=-16,0.=-21,3.=-16,4.=-11,5.=-16,6.=-16,7-=-48,7.=-91,74=-48,8.=-16,9.=-16",
    "font_fc_chakra_18": "16|-7=-10,.0=-6,.1=-22,.3=-4,.4=-3,.5=-4,.6=-4,.7=-9,.8=-4,.9=-4,0.=-6,3.=-4,4.=-3,5.=-4,6.=-4,7-=-13,7.=-24,74=-13,8.=-4,9.=-4",
    "font_fc_chakra_34": "16|-7=-19,.0=-11,.1=-41,.3=-8,.4=-5,.5=-8,.6=-8,.7=-16,.8=-8,.9=-8,0.=-11,3.=-8,4.=-5,5.=-8,6.=-8,7-=-24,7.=-46,74=-24,8.=-8,9.=-8",
    "font_fc_chakra_96": "23|-7=-37,.0=-21,.1=-80,.3=-16,.4=-11,.5=-16,.6=-16,.7=-32,.8=-16,.9=-16,0.=-21,3.=-16,4.=-11,5.=-16,6.=-16,7-=-48,7.=-91,74=-48,8.=-16,9.=-16",
    "font_fc_orbitron_112": "51|",
    "font_fc_orbitron_150": "68|",
    "font_fc_orbitron_18": "16|",
    "font_fc_orbitron_34": "16|",
    "font_fc_orbitron_96": "44|",
    "font_fc_oswald_112": "23|07=-5,17=-6,24=-19,41=-19,47=-16,57=-19,61=-9,62=-9,67=-12,69=-14,74=-31,87=-15,97=-21",
    "font_fc_oswald_150": "31|07=-5,17=-6,24=-19,41=-19,47=-16,57=-19,61=-9,62=-9,67=-12,69=-14,74=-31,87=-15,97=-21",
    "font_fc_oswald_18": "16|07=-1,17=-1,24=-4,41=-4,47=-4,57=-4,61=-2,62=-2,67=-3,69=-3,74=-7,87=-3,97=-5",
    "font_fc_oswald_34": "16|07=-6,17=-7,24=-4,41=-4,47=-9,57=-4,61=-2,62=-2,67=-5,69=-3,74=-18,87=-8,97=-7",
    "font_fc_oswald_96": "20|07=-5,17=-6,24=-18,41=-18,47=-16,57=-18,61=-9,62=-9,67=-12,69=-14,74=-31,87=-15,97=-21",
    "font_fc_oswald_l_150": "40|07=-13,17=-16,24=-3,41=-3,47=-18,57=-3,61=-1,62=-1,67=-10,69=-2,74=-36,87=-16,97=-11",
    "font_fc_outfit_112": "38|-1=-47,-2=-28,-3=-46,-7=-63,.0=-25,.1=-67,.4=-31,.6=-25,.7=-54,.9=-45,0.=-25,02=-5,07=-34,17=-16,2-=-30,27=-26,3.=-25,37=-19,4.=-38,41=-35,42=-12,44=-19,45=-14,47=-41,49=-21,5.=-25,57=-28,61=-24,63=-9,67=-32,69=-19,7-=-48,7.=-54,70=-14,72=-11,73=-21,74=-35,76=-31,77=-13,78=-23,79=-17,81=-17,82=-8,83=-19,87=-29,89=-13,9.=-59,91=-11,92=-11,93=-9,94=-19,96=-26,97=-32,98=-17",
    "font_fc_outfit_150": "51|-1=-47,-2=-28,-3=-46,-7=-63,.0=-25,.1=-67,.4=-31,.6=-25,.7=-53,.9=-45,0.=-25,02=-5,07=-34,17=-16,2-=-30,27=-26,3.=-25,37=-19,4.=-38,41=-35,42=-12,44=-19,45=-14,47=-41,49=-21,5.=-25,57=-28,61=-24,63=-9,67=-32,69=-19,7-=-47,7.=-54,70=-14,72=-11,73=-21,74=-35,76=-31,77=-13,78=-23,79=-17,81=-17,82=-8,83=-19,87=-29,89=-13,9.=-59,91=-11,92=-11,93=-9,94=-19,96=-26,97=-32,98=-17",
    "font_fc_outfit_18": "16|-1=-17,-2=-10,-3=-17,-7=-23,.0=-9,.1=-24,.4=-11,.6=-9,.7=-19,.9=-16,0.=-9,02=-1,07=-12,17=-4,2-=-10,27=-9,3.=-9,37=-7,4.=-13,41=-12,42=-4,44=-7,45=-5,47=-15,49=-8,5.=-9,57=-10,61=-8,63=-3,67=-11,69=-7,7-=-17,7.=-20,70=-5,72=-4,73=-7,74=-12,76=-11,77=-5,78=-7,79=-6,81=-6,82=-3,83=-6,87=-10,89=-4,9.=-22,91=-3,92=-4,93=-3,94=-6,96=-9,97=-11,98=-5",
    "font_fc_outfit_34": "16|-1=-32,-2=-18,-3=-32,-7=-44,.0=-16,.1=-46,.4=-20,.6=-17,.7=-36,.9=-30,0.=-16,02=-2,07=-22,17=-8,2-=-20,27=-16,3.=-17,37=-13,4.=-25,41=-23,42=-8,44=-13,45=-9,47=-28,49=-15,5.=-17,57=-18,61=-16,63=-7,67=-21,69=-13,7-=-32,7.=-38,70=-10,72=-7,73=-14,74=-23,76=-21,77=-9,78=-14,79=-11,81=-11,82=-5,83=-12,87=-18,89=-8,9.=-41,91=-7,92=-7,93=-6,94=-12,96=-17,97=-21,98=-10",
    "font_fc_outfit_96": "33|-1=-46,-2=-28,-3=-45,-7=-63,.0=-25,.1=-66,.4=-31,.6=-25,.7=-53,.9=-45,0.=-25,02=-4,07=-34,17=-16,2-=-30,27=-25,3.=-25,37=-19,4.=-38,41=-34,42=-12,44=-19,45=-14,47=-41,49=-21,5.=-25,57=-28,61=-24,63=-9,67=-32,69=-19,7-=-47,7.=-54,70=-14,72=-11,73=-21,74=-34,76=-31,77=-13,78=-22,79=-16,81=-16,82=-8,83=-19,87=-28,89=-13,9.=-58,91=-11,92=-11,93=-9,94=-19,96=-26,97=-32,98=-16",}

# 等幅数字 (機能画面用)。JetBrains Mono は全数字が同じ進幅 (tabular)。
# アプリアイコン (Material Symbols Rounded, Apache-2.0)。PUA コードポイント。
ICON_FONT_TTF = REPO / "tools" / "fonts" / "icons" / "MaterialSymbolsRounded.ttf"
# mic, music_note, timer, directions_walk, exposure_plus_1, alarm,
# notifications, settings, apps, timer_play, smart_toy, bedtime, hiking
ICON_FONT_RANGES = ("0xE31D,0xE405,0xE425,0xE536,0xE50A,0xE800,0xE855,"
                    "0xE7F5,0xE8B8,0xE5C3,0xF4BA,0xF06C,0xF159")

NUM_FONT_TTF = REPO / "tools" / "fonts" / "num" / "JetBrainsMono-Bold.ttf"
NUM_FONT_SIZES = (40, 72)

# lv_font_conv の起動コマンド (main() で解決)。
# 環境変数 LV_FONT_CONV → PATH → 開発環境の npx キャッシュの順で探す。
LV_FONT_CONV: list[str] = []
NUM_SYMBOLS = "0123456789:.,-/% "

STRING_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')

def patch_face_font_metrics(path: pathlib.Path, name: str) -> None:
    """サブセット生成で変わる line_height/base_line を ASCII 全量時代の値に戻す。

    lv_font_conv は line_height/base_line を「含めたグリフの最大高さ」から
    計算するため、グリフを絞ると値が小さくなる (oswald 150: 165/29→126/3)。
    LVGL はこの 2 値でラベル内のベースラインを決めるので、そのままだと
    数字盤の文字が十数 px ずれる。FACE_FONT_METRICS の値に書き戻して
    見た目を現行と一致させる。
    """
    if name not in FACE_FONT_METRICS:
        sys.exit(f"FACE_FONT_METRICS に {name} が無い (line_height 復元不能)")
    line_height, base_line = FACE_FONT_METRICS[name]
    text = path.read_text(encoding="utf-8")
    text, n1 = re.subn(r"\.line_height = \d+,", f".line_height = {line_height},",
                       text)
    text, n2 = re.subn(r"\.base_line = \d+,", f".base_line = {base_line},",
                       text)
    if n1 != 1 or n2 != 1:
        sys.exit(f"font_dsc の書き換えに失敗 ({path}: lh={n1} bl={n2})")
    path.write_text(text, encoding="utf-8")


def restore_face_font_kern(path: pathlib.Path, name: str) -> None:
    """サブセット生成で変わるカーニングを ASCII 全量時代の値に戻す。

    lv_font_conv は kern_scale と形式 (pairs/classes) をグリフ集合から
    再決定するため、数字ペアの送り幅が 1/16px 単位でずれる。すると
    中央寄せラベルの幅の丸めが変わり、数字が 1px 動く (sim 差分で確認)。
    FACE_FONT_KERN の値 (当時の kvalue と kern_scale) を書き戻すと
    LVGL の計算 ((kvalue * kern_scale) >> 4) が当時と完全に一致する。
    """
    ref = FACE_FONT_KERN.get(name)
    if ref is None:
        sys.exit(f"FACE_FONT_KERN に {name} が無い")
    scale_s, pair_s = ref.split("|", 1)
    kern_scale = int(scale_s)
    text = path.read_text(encoding="utf-8")
    ranges = [(int(a), int(b), int(c)) for a, b, c in re.findall(
        r"\.range_start = (\d+), \.range_length = (\d+), "
        r"\.glyph_id_start = (\d+)", text)]

    def gid(cp: int):
        for rs, rl, gs in ranges:
            if rs <= cp < rs + rl:
                return gs + (cp - rs)
        return None

    pairs = []
    for tok in filter(None, pair_s.split(",")):
        pair, val = tok.split("=")
        ga, gb = gid(ord(pair[0])), gid(ord(pair[1]))
        if ga is None or gb is None:
            sys.exit(f"{name}: 参照カーニングのグリフが生成物に無い ({pair})")
        if int(val):
            pairs.append((ga, gb, int(val)))
    pairs.sort(key=lambda p: (p[0] << 8) | p[1])

    if "kern_pair_glyph_ids[]" not in text:
        if pairs:
            sys.exit(f"{name}: kern 配列が生成されていない (参照 {len(pairs)} 組)")
        return  # 参照側も 0 組 (orbitron) — そのままで当時と同じ
    id_txt = ",\n".join("    %d, %d" % (a, b) for a, b, _ in pairs)
    val_txt = ", ".join(str(v) for _, _, v in pairs)
    text = re.sub(r"(kern_pair_glyph_ids\[\] =\s*\{).*?(\n\};)",
                  lambda m: m.group(1) + "\n" + id_txt + "\n};", text,
                  count=1, flags=re.S)
    text = re.sub(r"(kern_pair_values\[\] =\s*\{).*?(\n\};)",
                  lambda m: m.group(1) + "\n    " + val_txt + "\n};", text,
                  count=1, flags=re.S)
    text = re.sub(r"\.pair_cnt = \d+,", f".pair_cnt = {len(pairs)},", text)
    text = re.sub(r"\.kern_scale = \d+,", f".kern_scale = {kern_scale},",
                  text)
    path.write_text(text, encoding="utf-8")


def find_lv_font_conv() -> list[str]:
    """lv_font_conv の起動コマンドを返す。

    優先順: 環境変数 LV_FONT_CONV → PATH → 開発環境の npx キャッシュ
    (.toolchain/npm-cache / ~/.npm/_npx の lv_font_conv。--lv-font-name を
    サポートする 1.5.3+ のみ)。見つからなければ npm i -g lv_font_conv を促す。
    """
    env = os.environ.get("LV_FONT_CONV")
    if env:
        return env.split()
    exe = shutil.which("lv_font_conv")
    if exe:
        return [exe]
    node = shutil.which("node")
    if not node:
        sys.exit("lv_font_conv が見つかりません (npm i -g lv_font_conv か "
                 "LV_FONT_CONV=... を設定)")
    cands: list[pathlib.Path] = []
    for base in (REPO.parent / ".toolchain" / "npm-cache" / "_npx",
                 pathlib.Path.home() / ".npm" / "_npx"):
        if base.is_dir():
            cands += sorted(base.glob("*/node_modules/lv_font_conv/lv_font_conv.js"))
    for js in reversed(cands):  # 新しいキャッシュを優先
        try:
            help_text = subprocess.run(
                [node, str(js), "--help"], capture_output=True, text=True,
                check=False).stdout
        except OSError:
            continue
        if "--lv-font-name" in help_text:
            return [node, str(js)]
    sys.exit("lv_font_conv が見つかりません (npm i -g lv_font_conv か "
             "LV_FONT_CONV=... を設定)")


def collect_ui_chars(from_sources_only: bool = False) -> str:
    """UI ソースの文字列リテラルに含まれる非ASCII文字を全部拾う。

    from_sources_only=False: 全 JOYO 漢字を含む完全セット (本文用)。
    True: ソースリテラルに出る文字だけ (見出しフォントの絞り込み用)。
    """
    # "−"(U+2212) はマイナスボタンで使う。どこかのリテラルにあっても
    # 拾い漏れしないよう必ず入れる (BASE_RANGES の範囲外)。
    chars = (set(EXTRA_KANJI) | {"−"}) if from_sources_only else         (set(EXTRA_KANJI) | set(JOYO_KANJI) | {"−"})
    for path in sorted(
            [p for d in UI_SRC_SCAN for pat in ("*.cpp", "*.hpp")
             for p in d.rglob(pat)]):
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
    ap.add_argument("--font", help="日本語本文 TTF のパス (省略時は日本語系を生成しない)")
    ap.add_argument("--font-bold", help="見出し用 Bold TTF (font_jp_32 を生成)")
    ap.add_argument("--subset-from-sources", action="store_true",
                    help="UI ソースの文字列リテラルだけにグリフを絞る (font_jp_32 用)")
    ap.add_argument("--faces", action="store_true",
                    help="文字盤フォントを tools/fonts/face/ から生成する (T43: 数字記号のみ)")
    ap.add_argument("--num", action="store_true",
                    help="等幅数字フォントを tools/fonts/num/ から生成する")
    ap.add_argument("--jp-bin", action="store_true",
                    help="T24: assets 用の日本語 binfont (tools/fonts/bin/) と"
                         " フォールバック font_jp_min_20.c を生成する")
    ap.add_argument("--icons", action="store_true",
                    help="アプリアイコンフォント (Material Symbols Rounded 36px)")
    ap.add_argument("--binfont", metavar="TTF",
                    help="テーマ用の LVGL バイナリフォント (.bin, lv_binfont 形式)"
                         " を生成する --size と一緒に使う")
    ap.add_argument("--size", type=int, default=26,
                    help="--binfont のサイズ px (既定 26)")
    ap.add_argument("--binfont-out", metavar="PATH",
                    help="--binfont の出力 .bin (既定: <ttf名>_<size>.bin)")
    ap.add_argument("--num-ttf", default=str(NUM_FONT_TTF),
                    help="等幅数字フォントの TTF (既定: JetBrains Mono Bold)")
    ap.add_argument("--face-ttf-dir", default=str(FACE_FONT_TTF_DIR),
                    help="文字盤フォント TTF の場所")
    ap.add_argument("--face-chars", default=FACE_FONT_RANGE,
                    help="文字盤フォントのグリフ範囲 (既定: 0x2D-0x3A = -./0-9:)")
    ap.add_argument("--out", default=str(FONT_OUT))
    ap.add_argument("--bpp", default="4")
    args = ap.parse_args()

    global LV_FONT_CONV
    LV_FONT_CONV = find_lv_font_conv()

    # --binfont: 任意 TTF → テーマ用 lv_binfont (.bin)。
    # 既定範囲は jp 系と同じ (ASCII + 記号 + かな + 全角)。テーマの fonts
    # スロット (body/title/digits/digits_sm) で使う。
    if args.binfont:
        src = pathlib.Path(args.binfont)
        if not src.exists():
            sys.exit(f"missing TTF: {src}")
        outp = (pathlib.Path(args.binfont_out) if args.binfont_out
                else src.with_name(f"{src.stem}_{args.size}px.bin"))
        run([
            *LV_FONT_CONV,
            "--font", str(src),
            "--size", str(args.size),
            "--bpp", args.bpp,
            "--format", "bin",
            "--no-compress",
            "-r", BASE_RANGES,
            "-o", str(outp),
        ])
        print(f"binfont: {outp} ({outp.stat().st_size} B)")
        return 0

    if args.jp_bin:
        # T24: app から外した日本語フォント本体。assets の fonts/ に置く
        # .bin (lv_binfont) を tools/fonts/bin/ に生成し、フォールバックの
        # font_jp_min_20.c も作る。tools/mklfs_assets/ が bin を image に入れる。
        out = pathlib.Path(args.out)
        out.mkdir(parents=True, exist_ok=True)
        bin_out = REPO / "tools" / "fonts" / "bin"
        bin_out.mkdir(parents=True, exist_ok=True)
        symbols = collect_ui_chars()
        print(f"UI chars: {len(symbols)} codepoints (incl. EXTRA_KANJI)")
        ttf_med = REPO / "tools" / "fonts" / "jp" / "MPLUS1p-Medium.ttf"
        ttf_bold = REPO / "tools" / "fonts" / "jp" / "MPLUS1p-Bold.ttf"
        for size in (20, 26):
            run([
                *LV_FONT_CONV,
                "--font", str(ttf_med),
                "--size", str(size),
                "--bpp", args.bpp,
                "--format", "bin",
                "--no-compress",
                "-r", BASE_RANGES,
                "--symbols", symbols,
                "-o", str(bin_out / f"jp_{size}.bin"),
            ])
        sub = collect_ui_chars(from_sources_only=True)
        print(f"font_jp_32 chars: {len(sub)} codepoints")
        run([
            *LV_FONT_CONV,
            "--font", str(ttf_bold),
            "--size", "32",
            "--bpp", args.bpp,
            "--format", "bin",
            "--no-compress",
            "-r", BASE_RANGES,
            "--symbols", sub,
            "-o", str(bin_out / "jp_32.bin"),
        ])
        # フォールバックの最小フォント (ASCII+かな+記号のみ、漢字なし)。
        run([
            *LV_FONT_CONV,
            "--font", str(ttf_med),
            "--size", "20",
            "--bpp", args.bpp,
            "--format", "lvgl",
            "--lv-include", "lvgl.h",
            "-r", BASE_RANGES,
            "--lv-font-name", "font_jp_min_20",
            "-o", str(out / "font_jp_min_20.c"),
        ])
        return 0

    if not args.font and not args.faces and not args.num and not args.icons and not args.font_bold:
        ap.error("--font か --jp-bin か --faces か --num か --icons のどれかを指定して")

    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    if args.font:
        symbols = collect_ui_chars()
        print(f"UI chars: {len(symbols)} codepoints (incl. EXTRA_KANJI)")

        # 日本語フォント (範囲 + 文字列の和)
        for size in (20, 26):
            run([
                *LV_FONT_CONV,
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

    if args.font_bold:
        # 見出し 32px: グリフは ASCII + かな + ソースの漢字のみ (動的文字列には
        # 使わないので常用漢字は詰めない)。
        sub = collect_ui_chars(from_sources_only=True) if args.subset_from_sources \
            else collect_ui_chars()
        print(f"font_jp_32 chars: {len(sub)} codepoints")
        run([
            *LV_FONT_CONV,
            "--font", args.font_bold,
            "--size", "32",
            "--bpp", args.bpp,
            "--format", "lvgl",
            "--lv-include", "lvgl.h",
            "-r", BASE_RANGES,
            "--symbols", sub,
            "--lv-font-name", "font_jp_32",
            "-o", str(out / "font_jp_32.c"),
        ])

    if args.font:
        # 数字フォント
        for size in (56, 96):
            run([
                *LV_FONT_CONV,
                "--font", args.font,
                "--size", str(size),
                "--bpp", args.bpp,
                "--format", "lvgl",
                "--lv-include", "lvgl.h",
                "--symbols", DIGIT_SYMBOLS,
                "--lv-font-name", f"font_digits_{size}",
                "-o", str(out / f"font_digits_{size}.c"),
            ])

    if args.num:
        src = pathlib.Path(args.num_ttf)
        if not src.exists():
            sys.exit(f"missing TTF: {src}")
        for size in NUM_FONT_SIZES:
            run([
                *LV_FONT_CONV,
                "--font", str(src),
                "--size", str(size),
                "--bpp", args.bpp,
                "--format", "lvgl",
                "--lv-include", "lvgl.h",
                "--symbols", NUM_SYMBOLS,
                "--lv-font-name", f"font_num_{size}",
                "-o", str(out / f"font_num_{size}.c"),
            ])

    if args.icons:
        src = pathlib.Path(ICON_FONT_TTF)
        if not src.exists():
            sys.exit(f"missing TTF: {src}")
        run([
            *LV_FONT_CONV,
            "--font", str(src),
            "--size", "36",
            "--bpp", args.bpp,
            "--format", "lvgl",
            "--lv-include", "lvgl.h",
            "-r", ICON_FONT_RANGES,
            "--lv-font-name", "font_icon_36",
            "-o", str(out / "font_icon_36.c"),
        ])

    if args.faces:
        ttf_dir = pathlib.Path(args.face_ttf_dir)
        for fam, size, ttf in FACE_FONT_SPECS:
            src = ttf_dir / ttf
            if not src.exists():
                sys.exit(f"missing TTF: {src}")
            name = f"font_fc_{fam}_{size}"
            run([
                *LV_FONT_CONV,
                "--font", str(src),
                "--size", str(size),
                "--bpp", args.bpp,
                "--format", "lvgl",
                "--lv-include", "lvgl.h",
                "-r", args.face_chars,
                "--lv-font-name", name,
                "-o", str(out / f"{name}.c"),
            ])
            # line_height/base_line を ASCII 全量時代の値に戻す (見た目維持)。
            # TODO(T44): 数字の等幅化 (T3 の意図) は T16 の範囲変更以降
            # インデックスがずれて実際には効いていない。等幅化すると数字の
            # 送り幅が変わる (見た目が変わる) ので、別タスクで sim 差分を見て入れる。
            patch_face_font_metrics(out / f"{name}.c", name)
            restore_face_font_kern(out / f"{name}.c", name)

    print(f"done -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
