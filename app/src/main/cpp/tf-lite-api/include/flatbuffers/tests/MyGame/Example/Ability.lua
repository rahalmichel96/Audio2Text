--[[ MyGame.Example.Ability

  Automatically generated by the FlatBuffers compiler, do not modify.
  Or modify. I'm a message, not a cop.

  flatc version: 23.5.8

  Declared by  : //monster_test.fbs
  Rooting type : MyGame.Example.Monster (//monster_test.fbs)

--]]

local flatbuffers = require('flatbuffers')

local Ability = {}
local mt = {}

function Ability.New()
  local o = {}
  setmetatable(o, {__index = mt})
  return o
end

function mt:Init(buf, pos)
  self.view = flatbuffers.view.New(buf, pos)
end

function mt:Id()
  return self.view:Get(flatbuffers.N.Uint32, self.view.pos + 0)
end

function mt:Distance()
  return self.view:Get(flatbuffers.N.Uint32, self.view.pos + 4)
end

function Ability.CreateAbility(builder, id, distance)
  builder:Prep(4, 8)
  builder:PrependUint32(distance)
  builder:PrependUint32(id)
  return builder:Offset()
end

return Ability