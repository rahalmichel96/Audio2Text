#[ MyGame.OtherNameSpace.Unused
  Automatically generated by the FlatBuffers compiler, do not modify.
  Or modify. I'm a message, not a cop.

  flatc version: 23.5.8

  Declared by  : 
  Rooting type : MyGame.Example.Monster ()
]#

import flatbuffers

type Unused* = object of FlatObj
func a*(self: Unused): int32 =
  return Get[int32](self.tab, self.tab.Pos + 0)
func `a=`*(self: var Unused, n: int32): bool =
  return self.tab.Mutate(self.tab.Pos + 0, n)
proc UnusedCreate*(self: var Builder, a: int32): uoffset =
  self.Prep(4, 4)
  self.Prepend(a)
  return self.Offset()
