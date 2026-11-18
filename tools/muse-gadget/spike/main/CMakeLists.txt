# T49 spike: noise_core (S0) と chat 層 (S1) を参照してリンクさせる。
idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES noise_core muse_chat
)
