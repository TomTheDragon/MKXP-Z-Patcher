# Checksums of the standard VX Ace methods that the 3DS engine's native
# Sprite_Character#update stands for (engine/scene.c, engine/boot.rb).
#
#   ruby fast-checksums.rb <VX Ace game with the standard scripts> [--write]
#
# Prints the MKXPZ_STOCK table; --write puts it into engine/boot.rb. Only the
# code counts (comments and indentation are ignored), so translated script
# sets give the same checksums.
require 'zlib'

game = ARGV[0] or abort 'usage: ruby fast-checksums.rb <game folder> [--write]'
boot = File.join(__dir__, '..', 'engine', 'boot.rb')
src = File.read(boot)
helpers = src[/# >>> mkxpz-fast-helpers.*?# <<< mkxpz-fast-helpers/m] or abort 'helpers not found in boot.rb'
eval(helpers)

# the classes Marshal needs to read Scripts.rvdata2 (only strings inside)
scripts = Marshal.load(File.binread(File.join(game, 'Data', 'Scripts.rvdata2')))
sections = scripts.map { |_, name, code| [name.to_s.dup.force_encoding('UTF-8'), Zlib::Inflate.inflate(code).force_encoding('UTF-8')] }.to_h

defs = {
  'Sprite_Character' => %w[update update_bitmap graphic_changed? update_src_rect update_position update_other
                           update_balloon setup_new_effect],
  'Sprite_Base' => %w[update animation?],
  'Game_CharacterBase' => %w[screen_x screen_y screen_z shift_y jump_height object_character?],
  'Game_Map' => %w[adjust_x adjust_y loop_horizontal? loop_vertical? screen_tile_x screen_tile_y width height],
}
attrs = { 'Game_CharacterBase' => %w[tile_id character_name character_index pattern direction opacity blend_type
                                     bush_depth transparent animation_id balloon_id] }
engine = { 'Sprite' => %w[update x= y= z= opacity= blend_type= bush_depth= visible= src_rect] }

out = []
defs.each do |cls, names|
  text = sections[cls] or abort "no script section #{cls}"
  names.each do |n|
    i = text.lines.index { |l| l =~ /\A\s*def #{Regexp.escape(n)}(\s|\(|\z)/ } or abort "#{cls}##{n} not found"
    body = mkxpz_method_text(text, i + 1) or abort "#{cls}##{n}: no end"
    out << "  '#{cls}##{n}' => #{Zlib.crc32(mkxpz_norm(body))},"
  end
end
attrs.each { |cls, names| names.each { |n| out << "  '#{cls}##{n}' => :attr," } }
engine.each { |cls, names| names.each { |n| out << "  '#{cls}##{n}' => :c," } }
table = "MKXPZ_STOCK = {\n#{out.join("\n")}\n}"
puts table
if ARGV.include?('--write')
  new = src.sub(/^MKXPZ_STOCK = \{\n.*?^\}/m, table)
  abort 'MKXPZ_STOCK not found in boot.rb' if new == src && !src.include?(table)
  File.write(boot, new)
  puts "written to #{boot}"
end
