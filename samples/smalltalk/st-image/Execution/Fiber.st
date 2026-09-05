Fiber := Object [
    | handle |

    class fork: aBlock [
        | fiber |
        fiber := self new.
        fiber handle: (self spawn: aBlock).
        ^fiber
    ]

    class spawn: aBlock [
        <primitive: FiberSpawnPrimitive>
        ^InvalidArgumentError signal: 'Fiber entry must be a zero-argument block without a non-local return'
    ]

    class yield [
        <primitive: FiberYieldPrimitive>
        ^Error signal: 'Cannot suspend the current fiber'
    ]

    class sleepMilliseconds: duration [
        <primitive: FiberSleepPrimitive>
        ^InvalidArgumentError signal: 'Fiber delay must be a nonnegative 32-bit millisecond count'
    ]

    class join: identifier [
        <primitive: FiberJoinPrimitive>
        ^Error signal: 'Cannot join this fiber: invalid handle, consumed result or dependency cycle'
    ]

    class run [
        <primitive: FiberRunPrimitive>
        ^Error signal: 'Fiber scheduler did not complete successfully'
    ]

    class detach: identifier [
        <primitive: FiberDetachPrimitive>
        ^Error signal: 'Cannot detach a consumed fiber or one with a waiting joiner'
    ]

    handle: identifier [
        handle := identifier.
        ^self
    ]

    join [
        ^Fiber join: handle
    ]

    detach [
        Fiber detach: handle.
        handle := nil.
        ^self
    ]
]
