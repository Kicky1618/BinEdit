// Defines the complete Japanese and English UI string tables.
// Keeping text in one translation unit makes missing translations visible at
// compile time because both constexpr UiStrings initializers must be complete.

#include "Localization.h"

namespace {
// Field order mirrors UiStrings exactly. constexpr initialization prevents
// runtime allocation and guarantees string pointers remain valid indefinitely.
constexpr UiStrings japanese{
    L"無題",
    L"ファイル(&F)", L"新しいタブ(&N)\tCtrl+N", L"開く(&O)...\tCtrl+O", L"保存(&S)\tCtrl+S", L"名前を付けて保存(&A)...\tCtrl+Shift+S",
    L"タブを閉じる(&C)\tCtrl+W", L"タブを新しいウィンドウへ移動(&W)", L"終了(&X)\tAlt+F4",
    L"編集(&E)", L"元に戻す(&U)\tCtrl+Z", L"やり直す(&R)\tCtrl+Y", L"検索(&F)...\tCtrl+F", L"次を検索(&N)\tF3",
    L"選択範囲をマーク(&M)", L"すべてのマークを解除",
    L"ツール(&U)", L"ビット処理ツール(&B)...",
    L"文字形式(&C)",
    L"色(&L)", L"マーク色を設定(&M)...", L"現在のバイト値に色を設定(&B)...", L"現在のバイト値の色を解除",
    L"テーマ(&T)", L"システム(&S)", L"ライト(&L)", L"ダーク(&D)",
    L"言語(&G)", L"自動（システム）(&A)", L"日本語(&J)", L"English (&E)",
    L"ヘルプ(&H)", L"BinEdit について(&A)",
    L"オフセット", L"文字",
    L"%s%s    位置: 0x%llX    選択: %llu byte    %s    %llu byte    入力: %s    モード: %s",
    L"16進数", L"文字", L"挿入", L"上書き", L"検索中…", L"ファイルを読み込み中…", L"ファイルを保存中…",
    L"Direct3D 11 / Direct2D の初期化に失敗しました。",
    L"単一起動の同期オブジェクトを作成できませんでした。BinEdit を開始できません。",
    L"変更内容を保存しますか？", L"すべてのファイル", L"ファイルを開けませんでした", L"ファイルを保存できませんでした",
    L"編集内容を一時退避できませんでした", L"保存先への権限がありません。\n管理者権限で BinEdit を再起動して保存しますか？",
    L"管理者権限で次のファイルを上書きします。続行しますか？\n\n",
    L"管理者権限での保存に失敗しました。", L"BinEdit に未保存の変更があります。",
    L"未保存データの復元", L"異常終了時の未保存データが見つかりました。復元しますか？\n\n",
    L"ファイルが外部で変更されています。未保存の編集内容があります。\n\n再読み込みすると未保存の編集内容は失われます。",
    L"ファイルが外部で削除されたか移動されました。現在の内容を保持し、未保存として扱います。",
    L"OK", L"はい", L"いいえ", L"キャンセル", L"再読み込み", L"このまま続行",
    L"検索", L"検索文字列 / 16進パターン:", L"バイナリ", L"文字列", L"文字形式:", L"検索", L"キャンセル",
    L"一致するデータがありません。", L"検索処理を完了できませんでした。", L"検索パターンが空です。", L"16進パターンは '4D 5A ?? 00' の形式で入力してください。",
    L"各バイトは空白で区切ってください。", L"選択した文字形式では表現できない文字が含まれています。", L"文字列のエンコードに失敗しました。",
    L"検索パターンが長すぎます（最大 65536 文字）。", L"検索パターン用のメモリを確保できませんでした。",
    L"ビット処理ツール", L"演算",
    L"入力値", L"0～255 または 0x00～0xFF", L"オペランド",
    L"0～255 または 0x00～0xFF", L"計算結果", L"閉じる",
    L"算術結果は下位8ビットに折り返されます。", L"入力値は 0～255 の数値にしてください。", L"オペランドは 0～255 の数値にしてください。",
    L"シフト／ローテート回数は 0～7 にしてください。", L"0 で除算することはできません。",
    L"AND (&)", L"OR (|)", L"XOR (^)", L"反転 (~)",
    L"左シフト (<<)", L"右シフト (>>)", L"左ローテート", L"右ローテート",
    L"加算 (+)", L"減算 (-)", L"乗算 (*)", L"除算 (/)",
    L"色の選択", L"彩度と明度", L"現在の色", L"新しい色",
    L"赤 (R)", L"緑 (G)", L"青 (B)", L"HEX", L"矢印キーで微調整", L"適用",
    L"RGB は 0～255 で入力してください。", L"HEX は #RRGGBB 形式で入力してください。",
    L"BinEdit について", L"Win32 + Direct3D 11 + Direct2D 1.1", L"バージョン", L"ビルド", L"ビルド日時"
};

constexpr UiStrings english{
    L"Untitled",
    L"&File", L"&New tab\tCtrl+N", L"&Open...\tCtrl+O", L"&Save\tCtrl+S", L"Save &As...\tCtrl+Shift+S",
    L"&Close tab\tCtrl+W", L"Move tab to new &window", L"E&xit\tAlt+F4",
    L"&Edit", L"&Undo\tCtrl+Z", L"&Redo\tCtrl+Y", L"&Find...\tCtrl+F", L"Find &Next\tF3",
    L"&Mark selection", L"Clear all marks",
    L"T&ools", L"&Bit operation tool...",
    L"&Encoding",
    L"&Color", L"Set &mark color...", L"Color current &byte value...", L"Clear current byte color",
    L"&Theme", L"&System", L"&Light", L"&Dark",
    L"Lan&guage", L"&Automatic (system)", L"日本語 (&J)", L"&English",
    L"&Help", L"&About BinEdit",
    L"OFFSET", L"CHARACTERS",
    L"%s%s    Offset: 0x%llX    Selection: %llu byte    %s    %llu byte    Input: %s    Mode: %s",
    L"Hex", L"Characters", L"Insert", L"Overwrite", L"Searching…", L"Loading file…", L"Saving file…",
    L"Direct3D 11 / Direct2D initialization failed.",
    L"Could not create the single-instance synchronization object. BinEdit cannot start.",
    L"Save your changes?", L"All files", L"Could not open the file", L"Could not save the file",
    L"Could not stage the edited data", L"You do not have permission to save here.\nRestart BinEdit as administrator and save the file?",
    L"The elevated process will overwrite the following file. Continue?\n\n",
    L"The elevated save failed.", L"BinEdit has unsaved changes.",
    L"Recover unsaved data", L"Unsaved data from an interrupted session was found. Restore it?\n\n",
    L"The file was changed outside BinEdit and this tab has unsaved edits.\n\nReloading will discard the unsaved edits.",
    L"The file was deleted or moved outside BinEdit. Its current contents will be kept and marked as unsaved.",
    L"OK", L"Yes", L"No", L"Cancel", L"Reload", L"Continue",
    L"Find", L"Text / hexadecimal pattern:", L"Binary", L"Text", L"Encoding:", L"Find", L"Cancel",
    L"No matching data was found.", L"The search could not be completed.", L"The search pattern is empty.", L"Enter a hexadecimal pattern such as '4D 5A ?? 00'.",
    L"Separate each byte with a space.", L"The selected encoding cannot represent one or more characters.", L"Could not encode the search text.",
    L"The search pattern is too long (maximum 65,536 characters).", L"Could not allocate memory for the search pattern.",
    L"Bit operation tool", L"Operation",
    L"Input", L"0-255 or 0x00-0xFF", L"Operand",
    L"0-255 or 0x00-0xFF", L"Result", L"Close",
    L"Arithmetic results wrap to the low 8 bits.", L"Enter an input value from 0 through 255.", L"Enter an operand from 0 through 255.",
    L"Use a shift or rotation count from 0 through 7.", L"Division by zero is not allowed.",
    L"AND (&)", L"OR (|)", L"XOR (^)", L"NOT (~)",
    L"Shift left (<<)", L"Shift right (>>)", L"Rotate left", L"Rotate right",
    L"Add (+)", L"Subtract (-)", L"Multiply (*)", L"Divide (/)",
    L"Choose color", L"Saturation and value", L"Current", L"New",
    L"Red (R)", L"Green (G)", L"Blue (B)", L"HEX", L"Use arrow keys to adjust", L"Apply",
    L"Enter RGB values from 0 through 255.", L"Enter HEX as #RRGGBB.",
    L"About BinEdit", L"Win32 + Direct3D 11 + Direct2D 1.1", L"Version", L"Build", L"Built"
};
}

UiLanguage ResolveLanguage(LanguagePreference preference) {
    if (preference == LanguagePreference::Japanese) return UiLanguage::Japanese;
    if (preference == LanguagePreference::English) return UiLanguage::English;
    // UI language is the correct signal for application chrome; locale may differ
    // because users often format numbers/dates with another regional standard.
    const LANGID language = GetUserDefaultUILanguage();
    return PRIMARYLANGID(language) == LANG_JAPANESE ? UiLanguage::Japanese : UiLanguage::English;
}

const UiStrings& GetStrings(UiLanguage language) { return language == UiLanguage::Japanese ? japanese : english; }
