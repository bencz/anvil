ObjectMemory := Object [

    class collect [
        <primitive: MemoryCollectPrimitive>
        ^Error signal: 'Precise collection failed'
    ]
]
