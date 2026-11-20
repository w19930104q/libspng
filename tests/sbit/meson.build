sbit_test_exe = executable('sbit_test', 'sbit_test.c',
    dependencies : spng_dep)

sbit_images = [
    'g8_s5.png',
    'g8_nosbit.png',
    'g16_s13.png',
    'g16_s8.png',
    'ga8_s53.png',
    'ga16_s137.png',
    'rgb8_s564.png',
    'rgb8_nosbit.png',
    'rgb16_s13119.png',
    'rgba8_s5643.png',
    'rgba16_s137.png',
    'p8_s543.png',
    'p8_nosbit.png',
]

foreach image : sbit_images
    test('sbit_' + image.underscorify(), sbit_test_exe, args : files(image))
endforeach
