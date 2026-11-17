package dev.sasnews.amoledwatch.agent

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** フォールバック判定 (docs/muse-integration.md §5) のテスト。 */
class AiRoutingTest {

    private val openAi = AgentConfig(baseUrl = "https://api.example.com/v1", apiKey = "sk-x")
    private val meta = AgentConfig(metaApiKey = "LLM_x")
    private val muse = AgentConfig(museEnabled = true, museToken = "user:1-x")
    private val all = AgentConfig(
        baseUrl = "https://api.example.com/v1", apiKey = "sk-x",
        museEnabled = true, museToken = "user:1-x", metaApiKey = "LLM_x",
    )

    @Test
    fun sttPlanIsFreeFirst() {
        assertEquals(SttPlan.NONE, AiRouting.sttPlan(null))
        // 既定 = 自動（スマホ内 Vosk）。モデル未準備ならダウンロードを促す。
        assertEquals(SttPlan.VOSK_NOT_READY, AiRouting.sttPlan(AgentConfig()))
        assertEquals(SttPlan.VOSK, AiRouting.sttPlan(AgentConfig(), voskReady = true))
        // OpenAI キーがあっても、自動では有料/外部 STT に流さない。
        assertEquals(SttPlan.VOSK_NOT_READY, AiRouting.sttPlan(openAi))
        assertEquals(SttPlan.VOSK, AiRouting.sttPlan(openAi, voskReady = true))
        // Meta キーがあっても、自動では使わない (上級者向けの明示選択のみ)。
        assertEquals(SttPlan.VOSK_NOT_READY, AiRouting.sttPlan(meta))
        // 明示選択
        assertEquals(
            SttPlan.VOSK_NOT_READY,
            AiRouting.sttPlan(all.copy(sttMode = SttModes.VOSK)),
        )
        assertEquals(
            SttPlan.VOSK,
            AiRouting.sttPlan(all.copy(sttMode = SttModes.VOSK), voskReady = true),
        )
        assertEquals(SttPlan.OPENAI_BATCH, AiRouting.sttPlan(all.copy(sttMode = SttModes.OPENAI)))
        assertEquals(SttPlan.MUSE_REALTIME, AiRouting.sttPlan(meta.copy(sttMode = SttModes.MUSE_REALTIME)))
        // 有料キーが無ければ明示選択でも無料の Vosk に戻す。
        assertEquals(
            SttPlan.VOSK_NOT_READY,
            AiRouting.sttPlan(openAi.copy(sttMode = SttModes.MUSE_REALTIME)),
        )
        assertEquals(SttPlan.NONE, AiRouting.sttPlan(all.copy(sttMode = SttModes.NONE)))
    }

    @Test
    fun chatPlanFallsBack() {
        assertEquals(ChatPlan.NONE, AiRouting.chatPlan(null))
        assertEquals(ChatPlan.NONE, AiRouting.chatPlan(AgentConfig()))
        // Muse トークンなし → OpenAI 互換 chat/completions
        assertEquals(ChatPlan.OPENAI, AiRouting.chatPlan(openAi))
        // Muse ON + トークンあり → Muse
        assertEquals(ChatPlan.MUSE, AiRouting.chatPlan(muse))
        assertEquals(ChatPlan.MUSE, AiRouting.chatPlan(all))
        // Muse OFF でもトークンがあっても、OpenAI キーが無ければ返答経路なし
        assertEquals(ChatPlan.NONE, AiRouting.chatPlan(muse.copy(museEnabled = false)))
        // Muse ON でもトークンが空なら従来経路
        assertEquals(ChatPlan.OPENAI, AiRouting.chatPlan(openAi.copy(museEnabled = true)))
    }

    @Test
    fun nothingConfiguredOnlyWhenBothMissing() {
        assertTrue(AiRouting.nothingConfigured(null))
        // 既定は Vosk が未準備でも STT 経路として扱う (ダウンロード促しを出す)。
        assertFalse(AiRouting.nothingConfigured(AgentConfig()))
        assertFalse(AiRouting.nothingConfigured(meta, voskReady = true))
        assertFalse(AiRouting.nothingConfigured(openAi, voskReady = true))
        assertFalse(AiRouting.nothingConfigured(muse, voskReady = true))
        // STT も返答も切ったら「設定してください」に戻る
        assertTrue(
            AiRouting.nothingConfigured(
                AgentConfig(sttMode = SttModes.NONE, museEnabled = false),
            ),
        )
    }

    @Test
    fun describeIncludesBothPlans() {
        // 既定は無料構成 (Vosk)。上級者向けを明示すると Meta realtime になる。
        assertTrue(AiRouting.describe(AgentConfig(), voskReady = true).contains("VOSK"))
        assertTrue(AiRouting.describe(meta.copy(sttMode = SttModes.MUSE_REALTIME)).contains("MUSE_REALTIME"))
        assertTrue(AiRouting.describe(all).contains("MUSE"))
        assertEquals(AiRouting.VOSK_MODEL_MISSING, "設定で音声モデルをダウンロードしてください")
    }
}
