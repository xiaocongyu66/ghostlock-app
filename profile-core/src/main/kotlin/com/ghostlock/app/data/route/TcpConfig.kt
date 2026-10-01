package com.ghostlock.app.data.route

/**
 * `route.tcp_zerocopy` section. Native declares these three as plain (always
 * present) fields, so they stay non-null and are always emitted.
 */
data class TcpConfig(
    val attempts: UInt,
    val armSequence: UInt,
    val postReceiveHoldIterations: UInt,
    /* zc→stale-waiter overlay word offsets. 0 = native baked 6.6 geometry
     * (0x28/0x30); profiles for other kernel frame depths set these. */
    val taskWord: UInt = 0u,
    val lockWord: UInt = 0u,
) : RouteConfig {
    override fun entries(): List<Pair<String, ULong>> = listOf(
        "attempts" to attempts.toULong(),
        "arm_sequence" to armSequence.toULong(),
        "post_receive_hold_iterations" to postReceiveHoldIterations.toULong(),
        "task_word" to taskWord.toULong(),
        "lock_word" to lockWord.toULong(),
    )

    override fun apply(key: String, value: ULong): RouteConfig = when (key) {
        "attempts" -> copy(attempts = value.toUInt())
        "arm_sequence" -> copy(armSequence = value.toUInt())
        "post_receive_hold_iterations" -> copy(postReceiveHoldIterations = value.toUInt())
        "task_word" -> copy(taskWord = value.toUInt())
        "lock_word" -> copy(lockWord = value.toUInt())
        else -> this
    }

    companion object {
        val EMPTY = TcpConfig(0u, 0u, 0u)

        fun from(value: (String) -> Long?): TcpConfig = TcpConfig(
            attempts = value("execution.routes.tcp_zerocopy.attempts")?.toUInt() ?: 0u,
            armSequence = value("execution.routes.tcp_zerocopy.arm_sequence")?.toUInt() ?: 0u,
            postReceiveHoldIterations =
                value("execution.routes.tcp_zerocopy.post_receive_hold_iterations")?.toUInt() ?: 0u,
            taskWord = value("execution.routes.tcp_zerocopy.task_word")?.toUInt() ?: 0u,
            lockWord = value("execution.routes.tcp_zerocopy.lock_word")?.toUInt() ?: 0u,
        )
    }
}
